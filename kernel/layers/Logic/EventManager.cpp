#include "layers/Logic/EventManager.hpp"

#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TimeManager.hpp"

#include <klib/klib.h>

struct event_epoll_watch_t
{
    file_t*              file; // Weak: FileClosing detaches this before the target is freed.
    int32_t              descriptor;
    event_epoll_event_t  event;
    event_epoll_watch_t* next;
};

struct event_epoll_t
{
    inode_t              inode;
    EventManager*        manager;
    file_t*              file;
    event_epoll_watch_t* head;
    event_epoll_watch_t* tail;
    event_epoll_t*       next;
};

namespace
{
constexpr uint32_t NONBLOCK    = 04000;
constexpr uint32_t CLOEXEC     = 02000000;
constexpr uint64_t COUNTER_MAX = UINT64_MAX - 1;
struct event_counter_t
{
    inode_t       inode;
    spinlock_t    lock;
    uint64_t      value;
    bool          semaphore;
    EventManager* manager;
};
} // namespace

EventManager::EventManager(ResourceLayerCaps* resources, Scheduler* scheduler, VirtualFileSystem* vfs) : Resources(resources), SchedulerManager(scheduler), Vfs(vfs), Generation(0), EpollLock{}, Epolls(nullptr)
{
}

uint64_t EventManager::Sequence() const
{
    return __atomic_load_n(&Generation, __ATOMIC_ACQUIRE);
}

void EventManager::Notify()
{
    __atomic_add_fetch(&Generation, 1ULL, __ATOMIC_RELEASE);
    if (SchedulerManager != nullptr)
        SchedulerManager->WakeEventWaiters(&Generation);
}

uint64_t EventManager::Wait(uint64_t expected, uint64_t deadline)
{
    return SchedulerManager != nullptr ? SchedulerManager->WaitForEvent(&Generation, expected, deadline) : LINUX_ENOSYS;
}

uint64_t EventManager::Poll(event_pollfd_t* descriptors, size_t count, uint64_t deadline)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
        return LINUX_ENOSYS;
    ProcessManager* manager = Resources->processManager;
    for (;;)
    {
        const uint64_t sequence = Sequence();
        uint64_t       ready    = 0;
        {
            FileDescriptorGuard guard(manager->GetCurrentProcess());
            process_t*          process = manager->GetCurrentProcess();
            if (process == nullptr)
                return LINUX_ESRCH;
            for (size_t i = 0; i < count; ++i)
            {
                event_pollfd_t& descriptor = descriptors[i];
                descriptor.revents         = 0;
                if (descriptor.fd < 0)
                    continue;
                file_descriptor_t* fd   = manager->GetFileDescriptor(process, (uint32_t) descriptor.fd);
                const uint32_t     mask = fd != nullptr ? Vfs->Poll(static_cast<file_t*>(fd->file)) : EVENT_POLL_NVAL;
                descriptor.revents      = (uint16_t) (mask & (descriptor.events | EVENT_POLL_ERR | EVENT_POLL_HUP | EVENT_POLL_NVAL));
                if (descriptor.revents != 0)
                    ++ready;
            }
        }
        if (ready != 0 || deadline == 0)
            return ready;
        if (deadline != UINT64_MAX)
        {
            time_timespec_t time;
            if (Resources->timeManager == nullptr)
                return LINUX_ENOSYS;
            uint64_t result = Resources->timeManager->ReadClock(1, &time);
            if (result != 0)
                return result;
            const uint64_t now = (uint64_t) time.seconds * 1000000000 + time.nanoseconds;
            if (now >= deadline)
                return 0;
        }
        const uint64_t result = Wait(sequence, deadline);
        if (result != 0)
            return result;
    }
}

uint64_t EventManager::CreateCounter(uint32_t initial, uint32_t flags)
{
    if ((flags & ~(NONBLOCK | CLOEXEC | 1U)) != 0)
        return LINUX_EINVAL;
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
        return LINUX_ENOSYS;
    static const file_operations_t operations = {
            nullptr, nullptr, nullptr, CounterSeek, nullptr, nullptr, nullptr, CounterRelease, CounterPoll, CounterRead, CounterWrite,
    };
    event_counter_t* counter = static_cast<event_counter_t*>(kzalloc(sizeof(event_counter_t)));
    file_t*          file    = static_cast<file_t*>(kzalloc(sizeof(file_t)));
    if (counter == nullptr || file == nullptr)
    {
        kfree(counter);
        kfree(file);
        return LINUX_ENOMEM;
    }
    counter->inode.type = INODE_REGULAR;
    counter->value      = initial;
    counter->semaphore  = (flags & 1) != 0;
    counter->manager    = this;
    file->inode         = &counter->inode;
    file->operations    = &operations;
    file->privateData   = counter;
    file->statusFlags   = 2 | (flags & NONBLOCK); // O_RDWR; semaphore/CLOEXEC are not status flags
    file->refCount      = 1;
    int64_t descriptor;
    {
        FileDescriptorGuard guard(Resources->processManager->GetCurrentProcess());
        process_t*          process = Resources->processManager->GetCurrentProcess();
        descriptor                  = process != nullptr ? Resources->processManager->AddFileDescriptor(process, file, (flags & CLOEXEC) ? FD_FLAG_CLOEXEC : 0, VirtualFileSystem::ReleaseFileHandle) : (int64_t) LINUX_ESRCH;
    }
    if (descriptor < 0)
        Vfs->Close(file);
    return (uint64_t) descriptor;
}

uint32_t EventManager::CounterPoll(file_t* file)
{
    event_counter_t* counter = static_cast<event_counter_t*>(file->privateData);
    spinlock_acquire_preempt(&counter->lock);
    const uint32_t result = (counter->value != 0 ? EVENT_POLL_IN : 0) | (counter->value < COUNTER_MAX ? EVENT_POLL_OUT : 0);
    spinlock_release_preempt(&counter->lock);
    return result;
}

int64_t EventManager::CounterSeek(file_t* file, int64_t offset, int whence)
{
    (void) file;
    (void) offset;
    return whence >= 0 && whence <= 4 ? 0 : (int64_t) LINUX_EINVAL;
}

uint64_t EventManager::CounterRead(file_t* file, uint64_t address, uint64_t count)
{
    if (count < sizeof(uint64_t))
        return LINUX_EINVAL;
    event_counter_t* counter = static_cast<event_counter_t*>(file->privateData);
    for (;;)
    {
        const uint64_t       sequence = counter->manager->Sequence();
        user_memory_access_t access;
        if (!begin_user_memory(&access))
            return LINUX_ESRCH;
        spinlock_acquire_preempt(&counter->lock);
        uint64_t result = (uint64_t) -11; // EAGAIN
        if (counter->value != 0)
        {
            const uint64_t value = counter->semaphore ? 1 : counter->value;
            if (!safe_write_user_locked(&access, address, &value, sizeof(value)))
                result = LINUX_EFAULT;
            else
            {
                counter->value -= value;
                result = sizeof(value);
            }
        }
        spinlock_release_preempt(&counter->lock);
        end_user_memory(&access);
        if (result == sizeof(uint64_t))
            counter->manager->Notify();
        if (result != (uint64_t) -11 || (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & NONBLOCK))
            return result;
        result = counter->manager->Wait(sequence);
        if (result != 0)
            return result;
    }
}

uint64_t EventManager::CounterWrite(file_t* file, uint64_t address, uint64_t count)
{
    if (count != sizeof(uint64_t))
        return LINUX_EINVAL;
    uint64_t value;
    if (!safe_read_user(address, &value, sizeof(value)))
        return LINUX_EFAULT;
    if (value == UINT64_MAX)
        return LINUX_EINVAL;
    event_counter_t* counter = static_cast<event_counter_t*>(file->privateData);
    for (;;)
    {
        const uint64_t sequence = counter->manager->Sequence();
        spinlock_acquire_preempt(&counter->lock);
        const bool fits = value <= COUNTER_MAX - counter->value;
        if (fits)
            counter->value += value;
        spinlock_release_preempt(&counter->lock);
        if (fits)
        {
            counter->manager->Notify();
            return sizeof(value);
        }
        if (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & NONBLOCK)
            return (uint64_t) -11;
        const uint64_t result = counter->manager->Wait(sequence);
        if (result != 0)
            return result;
    }
}

void EventManager::CounterRelease(file_t* file)
{
    kfree(file->privateData);
}

file_t* EventManager::RetainDescriptor(int32_t fd)
{
    if (fd < 0)
        return nullptr;
    ProcessManager*     manager = Resources->processManager;
    FileDescriptorGuard guard(manager->GetCurrentProcess());
    file_descriptor_t*  descriptor = manager->GetFileDescriptor(manager->GetCurrentProcess(), (uint32_t) fd);
    file_t*             file       = descriptor != nullptr ? static_cast<file_t*>(descriptor->file) : nullptr;
    return Vfs->Retain(file) ? file : nullptr;
}

uint64_t EventManager::CreateEpoll(uint32_t flags)
{
    if ((flags & ~CLOEXEC) != 0)
        return LINUX_EINVAL;
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
        return LINUX_ENOSYS;
    static const file_operations_t operations = {
            nullptr, nullptr, nullptr, EpollSeek, nullptr, nullptr, nullptr, EpollRelease, EpollPoll, nullptr, nullptr,
    };
    event_epoll_t* epoll = static_cast<event_epoll_t*>(kzalloc(sizeof(event_epoll_t)));
    file_t*        file  = static_cast<file_t*>(kzalloc(sizeof(file_t)));
    if (epoll == nullptr || file == nullptr)
    {
        kfree(epoll);
        kfree(file);
        return LINUX_ENOMEM;
    }
    epoll->inode.type          = INODE_REGULAR;
    epoll->manager             = this;
    epoll->file                = file;
    file->inode                = &epoll->inode;
    file->operations           = &operations;
    file->privateData          = epoll;
    file->statusFlags          = 2;
    file->refCount             = 1;
    file->closeObserverContext = this;
    file->closeObserver        = FileClosing;
    spinlock_acquire_preempt(&EpollLock);
    epoll->next = Epolls;
    Epolls      = epoll;
    spinlock_release_preempt(&EpollLock);
    int64_t descriptor;
    {
        process_t*          process = Resources->processManager->GetCurrentProcess();
        FileDescriptorGuard guard(process);
        descriptor = process != nullptr ? Resources->processManager->AddFileDescriptor(process, file, (flags & CLOEXEC) ? FD_FLAG_CLOEXEC : 0, VirtualFileSystem::ReleaseFileHandle) : (int64_t) LINUX_ESRCH;
    }
    if (descriptor < 0)
        Vfs->Close(file);
    return (uint64_t) descriptor;
}

uint64_t EventManager::ControlEpoll(int32_t epfd, int32_t operation, int32_t fd, const event_epoll_event_t* event)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
        return LINUX_ENOSYS;
    file_t* file = RetainDescriptor(epfd);
    if (file == nullptr)
        return LINUX_EBADF;
    file_t*  target = RetainDescriptor(fd);
    uint64_t result = [&]() -> uint64_t
    {
        if (target == nullptr)
            return LINUX_EBADF;
        if (file->operations == nullptr || file->operations->Poll != EpollPoll || file == target)
            return LINUX_EINVAL;
        if (operation < 1 || operation > 3)
            return LINUX_EINVAL;
        if (target->operations != nullptr && target->operations->Poll == EpollPoll)
            return LINUX_EOPNOTSUPP;
        // Only sources with an explicit readiness callback can be watched.
        // Synchronous regular files use VFS's default readiness, but Linux
        // rejects them for epoll registration.
        if (target->operations == nullptr || target->operations->Poll == nullptr)
            return (uint64_t) -1; // EPERM
        event_epoll_watch_t* added = operation == 1 ? static_cast<event_epoll_watch_t*>(kzalloc(sizeof(event_epoll_watch_t))) : nullptr;
        if (operation == 1 && added == nullptr)
            return LINUX_ENOMEM;
        event_epoll_t* epoll = static_cast<event_epoll_t*>(file->privateData);
        spinlock_acquire_preempt(&EpollLock);
        event_epoll_watch_t** link     = &epoll->head;
        event_epoll_watch_t*  previous = nullptr;
        while (*link != nullptr && ((*link)->file != target || (*link)->descriptor != fd))
        {
            previous = *link;
            link     = &(*link)->next;
        }
        uint64_t status = 0;
        if (operation == 1 && *link != nullptr)
            status = (uint64_t) -17; // EEXIST
        else if (operation != 1 && *link == nullptr)
            status = LINUX_ENOENT;
        else if (operation != 2 && event == nullptr)
            status = LINUX_EINVAL;
        else if (operation != 2 && (event->events & ~0x3ffU) != 0)
            status = LINUX_EOPNOTSUPP;
        else if (operation == 1 && target->closeObserver != nullptr && (target->closeObserver != FileClosing || target->closeObserverContext != this))
            status = LINUX_EOPNOTSUPP;
        else if (operation == 1)
        {
            added->file       = target;
            added->descriptor = fd;
            added->event      = *event;
            if (epoll->tail != nullptr)
                epoll->tail->next = added;
            else
                epoll->head = added;
            epoll->tail = added;
            added       = nullptr;
            // The caller retains target until after unlocking. Once installed,
            // the observer is immutable through the target's last reference.
            if (target->closeObserver == nullptr)
            {
                target->closeObserverContext = this;
                target->closeObserver        = FileClosing;
            }
        }
        else if (operation == 3)
            (*link)->event = *event; // MOD
        else                         // DEL ignores the userspace event argument.
        {
            event_epoll_watch_t* removed = *link;
            *link                        = removed->next;
            if (epoll->tail == removed)
                epoll->tail = previous;
            kfree(removed);
        }
        spinlock_release_preempt(&EpollLock);
        kfree(added);
        if (status == 0)
            Notify();
        return status;
    }();
    // Final close invokes FileClosing, which takes EpollLock.
    Vfs->Close(target);
    Vfs->Close(file);
    return result;
}

uint64_t EventManager::ScanEpoll(event_epoll_t* epoll, event_epoll_event_t* events, size_t count)
{
    // Caller holds EpollLock. Final-close observers use the same lock, so weak
    // target pointers remain valid throughout each Poll callback.
    uint64_t              ready = 0;
    event_epoll_watch_t*  last  = epoll->tail;
    event_epoll_watch_t** link  = &epoll->head;
    while (*link != nullptr)
    {
        event_epoll_watch_t* watch = *link;
        const bool           end   = watch == last;
        const uint32_t       mask  = Vfs->Poll(watch->file) & (watch->event.events | EVENT_POLL_ERR | EVENT_POLL_HUP);
        if (mask != 0)
        {
            if (events == nullptr)
                return 1;
            events[ready++] = {mask, watch->event.data};
            // Move returned entries to the tail for round-robin fairness.
            if (watch != epoll->tail)
            {
                *link             = watch->next;
                epoll->tail->next = watch;
                epoll->tail       = watch;
                watch->next       = nullptr;
            }
            else
                link = &watch->next;
            if (ready == count)
                break;
        }
        else
            link = &watch->next;
        if (end)
            break;
    }
    return ready;
}

uint64_t EventManager::WaitEpoll(int32_t epfd, event_epoll_event_t* events, size_t count, uint64_t deadline)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
        return LINUX_ENOSYS;
    file_t* file = RetainDescriptor(epfd);
    if (file == nullptr)
        return LINUX_EBADF;
    uint64_t result = [&]() -> uint64_t
    {
        if (file->operations == nullptr || file->operations->Poll != EpollPoll)
            return LINUX_EINVAL;
        if (events == nullptr || count == 0)
            return LINUX_EINVAL;
        event_epoll_t* epoll = static_cast<event_epoll_t*>(file->privateData);
        for (;;)
        {
            const uint64_t sequence = Sequence();
            spinlock_acquire_preempt(&EpollLock);
            const uint64_t ready = ScanEpoll(epoll, events, count);
            spinlock_release_preempt(&EpollLock);
            if (ready != 0 || deadline == 0)
                return ready;
            if (deadline != UINT64_MAX)
            {
                time_timespec_t now;
                if (Resources->timeManager == nullptr)
                    return LINUX_ENOSYS;
                uint64_t status = Resources->timeManager->ReadClock(1, &now);
                if (status != 0)
                    return status;
                if ((uint64_t) now.seconds * 1000000000 + now.nanoseconds >= deadline)
                    return 0;
            }
            const uint64_t status = Wait(sequence, deadline);
            if (status != 0)
                return status;
        }
    }();
    Vfs->Close(file);
    return result;
}

uint32_t EventManager::EpollPoll(file_t* file)
{
    event_epoll_t* epoll   = static_cast<event_epoll_t*>(file->privateData);
    EventManager*  manager = epoll->manager;
    spinlock_acquire_preempt(&manager->EpollLock);
    const uint32_t ready = manager->ScanEpoll(epoll, nullptr, 0) ? EVENT_POLL_IN : 0;
    spinlock_release_preempt(&manager->EpollLock);
    return ready;
}

int64_t EventManager::EpollSeek(file_t*, int64_t, int)
{
    return -29; // ESPIPE
}

void EventManager::FileClosing(void* context, file_t* file)
{
    EventManager* manager = static_cast<EventManager*>(context);
    spinlock_acquire_preempt(&manager->EpollLock);
    event_epoll_t** instance = &manager->Epolls;
    while (*instance != nullptr)
    {
        event_epoll_t*        epoll    = *instance;
        event_epoll_watch_t** link     = &epoll->head;
        event_epoll_watch_t*  previous = nullptr;
        while (*link != nullptr)
        {
            event_epoll_watch_t* watch = *link;
            if (epoll->file == file || watch->file == file)
            {
                *link = watch->next;
                if (epoll->tail == watch)
                    epoll->tail = previous;
                kfree(watch);
            }
            else
            {
                previous = watch;
                link     = &watch->next;
            }
        }
        if (epoll->file == file)
            *instance = epoll->next;
        else
            instance = &epoll->next;
    }
    spinlock_release_preempt(&manager->EpollLock);
    manager->Notify();
}

void EventManager::EpollRelease(file_t* file)
{
    kfree(file->privateData);
}
