#include "layers/Logic/EventManager.hpp"

#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TimeManager.hpp"

#include <klib/klib.h>

struct event_epoll_watch_t
{
    file_close_observer_t observer;
    event_epoll_t*        owner;
    file_t*               file; // Weak: its intrusive close observer detaches before release/free.
    int32_t               descriptor;
    event_epoll_event_t   event;
    bool                  armed;
    bool                  pending;
    event_epoll_watch_t*  next;
};

struct event_epoll_t
{
    file_close_observer_t observer;
    uint64_t              sequence;
    inode_t               inode;
    EventManager*         manager;
    file_t*               file;
    event_epoll_watch_t*  head;
    event_epoll_watch_t*  tail;
    event_epoll_t*        next;
    uint64_t              topologyGeneration;
    size_t                topologyDepth;
    uint64_t              notificationGeneration;
};

namespace
{
constexpr uint32_t NONBLOCK        = 04000;
constexpr uint32_t CLOEXEC         = 02000000;
constexpr uint64_t COUNTER_MAX     = UINT64_MAX - 1;
constexpr uint32_t EPOLL_EDGE      = 1U << 31;
constexpr uint32_t EPOLL_ONCE      = 1U << 30;
constexpr uint32_t EPOLL_EXCLUSIVE = 1U << 28;
constexpr size_t   EPOLL_DEPTH     = 5;
struct event_counter_t
{
    inode_t       inode;
    spinlock_t    lock;
    uint64_t      value;
    bool          semaphore;
    EventManager* manager;
};
} // namespace

EventManager::EventManager(ResourceLayerCaps* resources, Scheduler* scheduler, VirtualFileSystem* vfs) : Resources(resources), SchedulerManager(scheduler), Vfs(vfs), Generation(0), TopologyGeneration(0), NotificationGeneration(0), EpollLock(VirtualFileSystem::ReadinessLock), Epolls(nullptr)
{
    NotificationLock = 0;
    Notifications    = nullptr;
}

uint64_t EventManager::Sequence() const
{
    return __atomic_load_n(&Generation, __ATOMIC_ACQUIRE);
}

void EventManager::Notify()
{
    __atomic_add_fetch(&Generation, 1ULL, __ATOMIC_RELEASE);

    if (SchedulerManager != nullptr)
    {
        SchedulerManager->WakeEventWaiters(&Generation);
    }
}

void EventManager::NotifySource(file_t* file, uint32_t events)
{
    spinlock_acquire_preempt(&EpollLock);
    ++NotificationGeneration;

    if (NotificationGeneration == 0)
    {
        for (event_epoll_t* epoll = Epolls; epoll != nullptr; epoll = epoll->next)
        {
            epoll->notificationGeneration = 0;
        }

        ++NotificationGeneration;
    }

    NotifySourceLocked(file, events, 0);
    spinlock_release_preempt(&EpollLock);
    Notify();
}

void EventManager::NotifySourceLocked(file_t* file, uint32_t events, size_t depth)
{
    if (depth >= EPOLL_DEPTH)
    {
        return;
    }

    size_t exclusiveCount = 0;
    for (auto* epoll = Epolls; epoll != nullptr; epoll = epoll->next)
    {
        for (auto* watch = epoll->head; watch != nullptr; watch = watch->next)
        {
            if (watch->file == file && watch->armed && (watch->event.events & EPOLL_EXCLUSIVE) && (events & (watch->event.events | EVENT_POLL_ERR | EVENT_POLL_HUP)))
            {
                ++exclusiveCount;
            }
        }
    }
    const size_t selected       = exclusiveCount != 0 ? ExclusiveGeneration++ % exclusiveCount : 0;
    size_t       exclusiveIndex = 0;
    for (event_epoll_t* epoll = Epolls; epoll != nullptr; epoll = epoll->next)
    {
        bool changed = false;

        for (event_epoll_watch_t* watch = epoll->head; watch != nullptr; watch = watch->next)
        {
            if (watch->file == file && watch->armed && (events & (watch->event.events | EVENT_POLL_ERR | EVENT_POLL_HUP)) != 0)
            {
                if ((watch->event.events & EPOLL_EXCLUSIVE) && exclusiveIndex++ != selected)
                {
                    continue;
                }
                watch->pending = true;
                changed        = true;
            }
        }

        if (changed && epoll->notificationGeneration != NotificationGeneration)
        {
            epoll->notificationGeneration = NotificationGeneration;
            WakeEpollLocked(epoll);
            NotifySourceLocked(epoll->file, EVENT_POLL_IN, depth + 1);
        }
    }
}

void EventManager::WakeEpollLocked(event_epoll_t* epoll)
{
    __atomic_add_fetch(&epoll->sequence, 1ULL, __ATOMIC_RELEASE);
    if (SchedulerManager != nullptr)
    {
        // Waiters release readiness/source locks before entering the scheduler.
        SchedulerManager->WakeEventWaiters(&epoll->sequence);
    }
}

uint64_t EventManager::Wait(uint64_t expected, uint64_t deadline)
{
    return SchedulerManager != nullptr ? SchedulerManager->WaitForEvent(&Generation, expected, deadline) : LINUX_ENOSYS;
}

uint64_t EventManager::Poll(event_pollfd_t* descriptors, size_t count, uint64_t deadline)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
    {
        return LINUX_ENOSYS;
    }
    if (count > SIZE_MAX / sizeof(file_t*))
    {
        return LINUX_EINVAL;
    }
    file_t** files = count != 0 ? static_cast<file_t**>(vmalloc(count * sizeof(file_t*))) : nullptr;
    if (count != 0 && files == nullptr)
    {
        return LINUX_ENOMEM;
    }
    if (count != 0)
    {
        memset(files, 0, count * sizeof(file_t*));
    }

    ProcessManager* manager = Resources->processManager;
    process_t*      process = manager->GetCurrentProcess();
    {
        FileDescriptorGuard guard(process);
        for (size_t i = 0; process != nullptr && i < count; ++i)
        {
            auto* descriptor = descriptors[i].fd >= 0 ? manager->GetFileDescriptor(process, (uint32_t) descriptors[i].fd) : nullptr;
            auto* file       = descriptor != nullptr ? static_cast<file_t*>(descriptor->file) : nullptr;
            if (Vfs->Retain(file))
            {
                files[i] = file;
            }
        }
    }

    const uint64_t result = [&]() -> uint64_t
    {
        if (process == nullptr)
        {
            return LINUX_ESRCH;
        }

        for (;;)
        {
            const uint64_t sequence = Sequence();
            uint64_t       ready    = 0;

            for (size_t i = 0; i < count; ++i)
            {
                auto& descriptor   = descriptors[i];
                descriptor.revents = 0;

                if (descriptor.fd < 0)
                {
                    continue;
                }

                const uint32_t mask = files[i] != nullptr ? Vfs->Poll(files[i]) : EVENT_POLL_NVAL;
                descriptor.revents  = (uint16_t) (mask & (descriptor.events | EVENT_POLL_ERR | EVENT_POLL_HUP | EVENT_POLL_NVAL));
                ready += descriptor.revents != 0;
            }

            if (ready != 0 || deadline == 0)
            {
                return ready;
            }

            if (deadline != UINT64_MAX)
            {
                time_timespec_t time;

                if (Resources->timeManager == nullptr)
                {
                    return LINUX_ENOSYS;
                }

                const uint64_t status = Resources->timeManager->ReadClock(1, &time);

                if (status != 0)
                {
                    return status;
                }

                if ((uint64_t) time.seconds * 1000000000 + time.nanoseconds >= deadline)
                {
                    return 0;
                }
            }

            const uint64_t status = Wait(sequence, deadline);

            if (status != 0)
            {
                return status;
            }
        }
    }();

    for (size_t i = 0; i < count; ++i)
    {
        Vfs->Close(files[i]);
    }

    if (files != nullptr)
    {
        vfree(files);
    }

    return result;
}

uint64_t EventManager::CreateCounter(uint32_t initial, uint32_t flags)
{
    if ((flags & ~(NONBLOCK | CLOEXEC | 1U)) != 0)
    {
        return LINUX_EINVAL;
    }

    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
    {
        return LINUX_ENOSYS;
    }

    static const file_operations_t operations = {
            nullptr, nullptr, nullptr, CounterSeek, nullptr, nullptr, nullptr, CounterRelease, CounterPoll, CounterRead, CounterWrite, CounterReadVector, CounterWriteVector,
    };
    event_counter_t* counter = static_cast<event_counter_t*>(kzalloc(sizeof(event_counter_t)));
    file_t*          file    = static_cast<file_t*>(kzalloc(sizeof(file_t)));

    if (counter == nullptr || file == nullptr)
    {
        kfree(counter);
        kfree(file);
        return LINUX_ENOMEM;
    }

    Vfs->InitializeAnonymousInode(&counter->inode, INODE_REGULAR, 0600);
    counter->value     = initial;
    counter->semaphore = (flags & 1) != 0;
    counter->manager   = this;
    file->inode        = &counter->inode;
    file->operations   = &operations;
    file->privateData  = counter;
    file->statusFlags  = 2 | (flags & NONBLOCK); // O_RDWR; semaphore/CLOEXEC are not status flags
    file->refCount     = 1;
    int64_t descriptor;
    {
        FileDescriptorGuard guard(Resources->processManager->GetCurrentProcess());
        process_t*          process = Resources->processManager->GetCurrentProcess();
        descriptor                  = process != nullptr ? Resources->processManager->AddFileDescriptor(process, file, (flags & CLOEXEC) ? FD_FLAG_CLOEXEC : 0, dispatcher_vfs_file_operations()) : (int64_t) LINUX_ESRCH;
    }

    if (descriptor < 0)
    {
        Vfs->Close(file);
    }

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
    {
        return LINUX_EINVAL;
    }

    const file_iovec_t vector = {address, count};
    return CounterReadVector(file, &vector, 1);
}

uint64_t EventManager::CounterReadVector(file_t* file, const file_iovec_t* vectors, size_t count)
{
    size_t available = 0;

    for (size_t i = 0; i < count && available < sizeof(uint64_t); ++i)
    {
        const uint64_t remaining = sizeof(uint64_t) - available;
        available += vectors[i].length < remaining ? vectors[i].length : remaining;
    }

    if (available < sizeof(uint64_t))
    {
        return LINUX_EINVAL;
    }

    event_counter_t* counter = static_cast<event_counter_t*>(file->privateData);

    for (;;)
    {
        const uint64_t       sequence = counter->manager->Sequence();
        user_memory_access_t access;

        if (!begin_user_memory(&access))
        {
            return LINUX_ESRCH;
        }

        size_t checked = 0;

        for (size_t i = 0; i < count && checked < sizeof(uint64_t); ++i)
        {
            const size_t remaining = sizeof(uint64_t) - checked;
            const size_t bytes     = vectors[i].length < remaining ? (size_t) vectors[i].length : remaining;

            if (bytes != 0 && !safe_user_memory_accessible_locked(&access, vectors[i].address, bytes, true))
            {
                break;
            }

            checked += bytes;
        }

        spinlock_acquire_preempt(&counter->lock);
        uint64_t result = (uint64_t) -11; // EAGAIN

        if (counter->value != 0)
        {
            const uint64_t value = counter->semaphore ? 1 : counter->value;

            size_t copied = 0;

            for (size_t i = 0; i < count && checked == sizeof(value) && copied < sizeof(value); ++i)
            {
                const size_t remaining = sizeof(value) - copied;
                const size_t bytes     = vectors[i].length < remaining ? (size_t) vectors[i].length : remaining;

                if (bytes != 0 && !safe_write_user_locked(&access, vectors[i].address, reinterpret_cast<const uint8_t*>(&value) + copied, bytes))
                {
                    break;
                }

                copied += bytes;
            }

            if (copied == sizeof(value))
            {
                counter->value -= value;
                result = sizeof(value);
            }
            else
            {
                result = LINUX_EFAULT;
            }
        }

        spinlock_release_preempt(&counter->lock);
        end_user_memory(&access);

        if (result == sizeof(uint64_t))
        {
            counter->manager->NotifySource(file, EVENT_POLL_OUT);
        }

        if (result != (uint64_t) -11 || (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & NONBLOCK))
        {
            return result;
        }

        result = counter->manager->Wait(sequence);

        if (result != 0)
        {
            return result;
        }
    }
}

uint64_t EventManager::CounterWrite(file_t* file, uint64_t address, uint64_t count)
{
    if (count != sizeof(uint64_t))
    {
        return LINUX_EINVAL;
    }

    uint64_t value;

    if (!safe_read_user(address, &value, sizeof(value)))
    {
        return LINUX_EFAULT;
    }

    if (value == UINT64_MAX)
    {
        return LINUX_EINVAL;
    }

    event_counter_t* counter = static_cast<event_counter_t*>(file->privateData);

    for (;;)
    {
        const uint64_t sequence = counter->manager->Sequence();
        spinlock_acquire_preempt(&counter->lock);
        const bool fits = value <= COUNTER_MAX - counter->value;

        if (fits)
        {
            counter->value += value;
        }

        spinlock_release_preempt(&counter->lock);

        if (fits)
        {
            counter->manager->NotifySource(file, EVENT_POLL_IN);
            return sizeof(value);
        }

        if (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & NONBLOCK)
        {
            return (uint64_t) -11;
        }

        const uint64_t result = counter->manager->Wait(sequence);

        if (result != 0)
        {
            return result;
        }
    }
}

void EventManager::CounterRelease(file_t* file)
{
    kfree(file->privateData);
}

uint64_t EventManager::CounterWriteVector(file_t* file, const file_iovec_t* vectors, size_t count)
{
    // Linux eventfd has a scalar write callback: writev processes each nonempty
    // vector as a separate eight-byte increment and returns partial progress.
    uint64_t total = 0;

    for (size_t i = 0; i < count; ++i)
    {
        if (vectors[i].length == 0)
        {
            continue;
        }

        const uint64_t result = CounterWrite(file, vectors[i].address, vectors[i].length);

        if ((int64_t) result < 0)
        {
            return total != 0 ? total : result;
        }

        total += result;
    }

    return total;
}

file_t* EventManager::RetainDescriptor(int32_t fd)
{
    if (fd < 0)
    {
        return nullptr;
    }

    ProcessManager*     manager = Resources->processManager;
    FileDescriptorGuard guard(manager->GetCurrentProcess());
    file_descriptor_t*  descriptor = manager->GetFileDescriptor(manager->GetCurrentProcess(), (uint32_t) fd);
    file_t*             file       = descriptor != nullptr ? static_cast<file_t*>(descriptor->file) : nullptr;
    return Vfs->Retain(file) ? file : nullptr;
}

uint64_t EventManager::CreateEpoll(uint32_t flags)
{
    if ((flags & ~CLOEXEC) != 0)
    {
        return LINUX_EINVAL;
    }

    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
    {
        return LINUX_ENOSYS;
    }

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

    Vfs->InitializeAnonymousInode(&epoll->inode, INODE_REGULAR, 0600);
    epoll->manager          = this;
    epoll->file             = file;
    file->inode             = &epoll->inode;
    file->operations        = &operations;
    file->privateData       = epoll;
    file->statusFlags       = 2;
    file->refCount          = 1;
    epoll->observer.Closing = EpollClosing;
    epoll->observer.context = epoll;
    spinlock_acquire_preempt(&EpollLock);
    Vfs->ObserveFileLocked(file, &epoll->observer);
    epoll->next = Epolls;
    Epolls      = epoll;
    spinlock_release_preempt(&EpollLock);
    int64_t descriptor;
    {
        process_t*          process = Resources->processManager->GetCurrentProcess();
        FileDescriptorGuard guard(process);
        descriptor = process != nullptr ? Resources->processManager->AddFileDescriptor(process, file, (flags & CLOEXEC) ? FD_FLAG_CLOEXEC : 0, dispatcher_vfs_file_operations()) : (int64_t) LINUX_ESRCH;
    }

    if (descriptor < 0)
    {
        Vfs->Close(file);
    }

    return (uint64_t) descriptor;
}

bool EventManager::CheckEpollDepth(event_epoll_t* epoll, event_epoll_t* owner, event_epoll_t* target, size_t depth)
{
    if (depth > EPOLL_DEPTH)
    {
        return false;
    }

    if (epoll->topologyGeneration == TopologyGeneration && epoll->topologyDepth >= depth)
    {
        return true;
    }

    epoll->topologyGeneration = TopologyGeneration;
    epoll->topologyDepth      = depth;

    for (event_epoll_watch_t* watch = epoll->head; watch != nullptr; watch = watch->next)
    {
        if (watch->file->operations->Poll == EpollPoll && !CheckEpollDepth(static_cast<event_epoll_t*>(watch->file->privateData), owner, target, depth + 1))
        {
            return false;
        }
    }

    return epoll != owner || CheckEpollDepth(target, owner, target, depth + 1);
}

uint64_t EventManager::ControlEpoll(int32_t epfd, int32_t operation, int32_t fd, const event_epoll_event_t* event)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
    {
        return LINUX_ENOSYS;
    }

    file_t* file;
    file_t* target;
    {
        FileDescriptorGuard descriptors(Resources->processManager->GetCurrentProcess());
        file   = RetainDescriptor(epfd);
        target = RetainDescriptor(fd);
    }

    if (file == nullptr)
    {
        Vfs->Close(target);
        return LINUX_EBADF;
    }

    uint64_t result = [&]() -> uint64_t
    {
        if (target == nullptr)
        {
            return LINUX_EBADF;
        }

        if (file->operations == nullptr || file->operations->Poll != EpollPoll || file == target)
        {
            return LINUX_EINVAL;
        }

        if (operation < 1 || operation > 3)
        {
            return LINUX_EINVAL;
        }

        // Only sources with an explicit readiness callback can be watched.
        // Synchronous regular files use VFS's default readiness, but Linux
        // rejects them for epoll registration.

        if (target->operations == nullptr || target->operations->Poll == nullptr)
        {
            return (uint64_t) -1; // EPERM
        }
        if (target->operations->Poll == EpollPoll && static_cast<event_epoll_t*>(target->privateData)->manager != this)
        {
            return LINUX_EOPNOTSUPP;
        }
        event_epoll_watch_t* added = operation == 1 ? static_cast<event_epoll_watch_t*>(kzalloc(sizeof(event_epoll_watch_t))) : nullptr;

        if (operation == 1 && added == nullptr)
        {
            return LINUX_ENOMEM;
        }

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
        {
            status = (uint64_t) -17; // EEXIST
        }
        else if (operation != 1 && *link == nullptr)
        {
            status = LINUX_ENOENT;
        }
        else if (operation != 2 && event == nullptr)
        {
            status = LINUX_EINVAL;
        }
        else if (operation != 2 && ((event->events & EPOLL_EXCLUSIVE) || (operation == 3 && ((*link)->event.events & EPOLL_EXCLUSIVE)))
                 && (operation != 1 || target->operations->Poll == EpollPoll || (event->events & ~(EVENT_POLL_IN | EVENT_POLL_OUT | EVENT_POLL_ERR | EVENT_POLL_HUP | EPOLL_EDGE | EPOLL_EXCLUSIVE | (1U << 29))) != 0))
        {
            status = LINUX_EINVAL;
        }
        else if (operation != 2 && (event->events & ~(0x3ffU | 0x2000U | EPOLL_EDGE | EPOLL_ONCE | EPOLL_EXCLUSIVE)) != 0)
        {
            status = LINUX_EOPNOTSUPP;
        }

        if (status == 0 && operation == 1 && target->operations->Poll == EpollPoll)
        {
            ++TopologyGeneration;

            if (TopologyGeneration == 0)
            {
                for (event_epoll_t* instance = Epolls; instance != nullptr; instance = instance->next)
                {
                    instance->topologyGeneration = 0;
                }

                ++TopologyGeneration;
            }

            for (event_epoll_t* instance = Epolls; instance != nullptr; instance = instance->next)
            {
                if (!CheckEpollDepth(instance, epoll, static_cast<event_epoll_t*>(target->privateData), 1))
                {
                    status = (uint64_t) -40; // ELOOP
                    break;
                }
            }
        }

        if (status == 0 && operation == 1)
        {
            added->file             = target;
            added->descriptor       = fd;
            added->event            = *event;
            added->armed            = true;
            added->pending          = true;
            added->owner            = epoll;
            added->observer.Closing = WatchClosing;
            added->observer.context = added;
            Vfs->ObserveFileLocked(target, &added->observer);

            if (epoll->tail != nullptr)
            {
                epoll->tail->next = added;
            }
            else
            {
                epoll->head = added;
            }
            epoll->tail = added;
            added       = nullptr;
        }
        else if (status == 0 && operation == 3)
        {
            (*link)->event   = *event;
            (*link)->armed   = true;
            (*link)->pending = true;
        }
        else if (status == 0) // DEL ignores the userspace event argument.
        {
            event_epoll_watch_t* removed = *link;
            Vfs->UnobserveFileLocked(removed->file, &removed->observer);
            *link                        = removed->next;

            if (epoll->tail == removed)
            {
                epoll->tail = previous;
            }

            kfree(removed);
        }

        if (status == 0)
        {
            WakeEpollLocked(epoll);
        }

        spinlock_release_preempt(&EpollLock);
        kfree(added);

        if (status == 0)
        {
            NotifySource(file, EVENT_POLL_IN);
        }

        return status;
    }();
    // Final close detaches readiness observers under the same graph lock.
    Vfs->Close(target);
    Vfs->Close(file);
    return result;
}

uint32_t EventManager::WatchReadiness(event_epoll_watch_t* watch)
{
    if (!watch->armed || ((watch->event.events & (EPOLL_EDGE | EPOLL_EXCLUSIVE)) != 0 && !watch->pending))
    {
        return 0;
    }

    const uint32_t readiness = watch->file->operations->Poll == EpollPoll ? (ScanEpoll(static_cast<event_epoll_t*>(watch->file->privateData), nullptr, 0, 0) != 0 ? EVENT_POLL_IN : 0) : Vfs->Poll(watch->file);
    const uint32_t result    = readiness & (watch->event.events | EVENT_POLL_ERR | EVENT_POLL_HUP);
    if (result == 0)
    {
        watch->pending = false;
    }
    return result;
}

uint64_t EventManager::ScanEpoll(event_epoll_t* epoll, const user_memory_access_t* access, uint64_t address, size_t count)
{
    // Caller holds EpollLock. Final-close observers use the same lock, so weak
    // target pointers remain valid throughout each Poll callback.
    uint64_t             ready  = 0;
    event_epoll_watch_t* last   = epoll->tail;
    event_epoll_watch_t**  link = &epoll->head;

    while (*link != nullptr)
    {
        event_epoll_watch_t* watch = *link;
        const bool           end   = watch == last;
        const uint32_t       mask  = WatchReadiness(watch);

        if (mask != 0)
        {
            if (access == nullptr)
            {
                return 1;
            }

            const event_epoll_event_t event = {mask, watch->event.data};

            if (!safe_write_user_locked(access, address + ready * sizeof(event), &event, sizeof(event)))
            {
                return ready != 0 ? ready : LINUX_EFAULT;
            }

            ++ready;
            watch->pending = (watch->event.events & EPOLL_EXCLUSIVE) != 0 && (watch->event.events & EPOLL_EDGE) == 0;

            if ((watch->event.events & EPOLL_ONCE) != 0)
            {
                watch->armed = false;
            }

            // Move returned entries to the tail for round-robin fairness.

            if (watch != epoll->tail)
            {
                *link             = watch->next;
                epoll->tail->next = watch;
                epoll->tail       = watch;
                watch->next       = nullptr;
            }
            else
            {
                link = &watch->next;
            }

            if (ready == count)
            {
                break;
            }
        }
        else
        {
            link = &watch->next;
        }

        if (end)
        {
            break;
        }
    }

    return ready;
}

uint64_t EventManager::WaitEpoll(int32_t epfd, uint64_t address, size_t count, uint64_t deadline)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
    {
        return LINUX_ENOSYS;
    }

    file_t* file = RetainDescriptor(epfd);

    if (file == nullptr)
    {
        return LINUX_EBADF;
    }

    uint64_t result = [&]() -> uint64_t
    {
        if (file->operations == nullptr || file->operations->Poll != EpollPoll)
        {
            return LINUX_EINVAL;
        }

        if (count == 0)
        {
            return LINUX_EINVAL;
        }

        event_epoll_t* epoll = static_cast<event_epoll_t*>(file->privateData);

        for (;;)
        {
            const uint64_t       sequence = __atomic_load_n(&epoll->sequence, __ATOMIC_ACQUIRE);
            user_memory_access_t access;

            // Memory precedes EpollLock, which precedes a source's state lock.
            // Release all three before parking or dropping a file reference.
            if (!begin_user_memory(&access))
            {
                return LINUX_ESRCH;
            }

            spinlock_acquire_preempt(&EpollLock);
            const uint64_t ready = ScanEpoll(epoll, &access, address, count);
            const bool     more  = (int64_t) ready > 0 && ScanEpoll(epoll, nullptr, 0, 0) != 0;
            spinlock_release_preempt(&EpollLock);
            end_user_memory(&access);

            if (more)
            {
                NotifySource(file, EVENT_POLL_IN);
            }

            if (ready != 0 || deadline == 0)
            {
                return ready;
            }

            if (deadline != UINT64_MAX)
            {
                time_timespec_t now;

                if (Resources->timeManager == nullptr)
                {
                    return LINUX_ENOSYS;
                }

                uint64_t status = Resources->timeManager->ReadClock(1, &now);

                if (status != 0)
                {
                    return status;
                }

                if ((uint64_t) now.seconds * 1000000000 + now.nanoseconds >= deadline)
                {
                    return 0;
                }
            }

            const uint64_t status = SchedulerManager != nullptr ? SchedulerManager->WaitForEvent(&epoll->sequence, sequence, deadline) : LINUX_ENOSYS;

            if (status != 0)
            {
                return status;
            }
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
    const uint32_t ready = manager->ScanEpoll(epoll, nullptr, 0, 0) ? EVENT_POLL_IN : 0;
    spinlock_release_preempt(&manager->EpollLock);
    return ready;
}

int64_t EventManager::EpollSeek(file_t*, int64_t, int)
{
    return -29; // ESPIPE
}

void EventManager::RemoveEpollWatchLocked(event_epoll_watch_t* watch)
{
    auto* epoll                   = watch->owner;
    auto** link                   = &epoll->head;
    event_epoll_watch_t* previous = nullptr;
    while (*link != nullptr && *link != watch)
    {
        previous = *link;
        link     = &(*link)->next;
    }
    if (*link == nullptr)
    {
        return;
    }
    *link = watch->next;
    if (epoll->tail == watch)
    {
        epoll->tail = previous;
    }
    Vfs->UnobserveFileLocked(watch->file, &watch->observer);
    kfree(watch);
}

void EventManager::WatchClosing(file_close_observer_t* observer, file_t*)
{
    auto* watch   = static_cast<event_epoll_watch_t*>(observer->context);
    auto* epoll   = watch->owner;
    auto* manager = epoll->manager;
    manager->RemoveEpollWatchLocked(watch);
    manager->WakeEpollLocked(epoll);
    manager->Notify();
}

void EventManager::EpollClosing(file_close_observer_t* observer, file_t*)
{
    auto* epoll   = static_cast<event_epoll_t*>(observer->context);
    auto* manager = epoll->manager;
    while (epoll->head != nullptr)
    {
        manager->RemoveEpollWatchLocked(epoll->head);
    }
    auto** link = &manager->Epolls;
    while (*link != nullptr && *link != epoll)
    {
        link = &(*link)->next;
    }
    if (*link != nullptr)
    {
        *link = epoll->next;
    }
    manager->Notify();
}

void EventManager::EpollRelease(file_t* file)
{
    kfree(file->privateData);
}
