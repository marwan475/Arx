#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TimeManager.hpp"

#include <klib/klib.h>

namespace
{
constexpr uint32_t NONBLOCK      = 04000;
constexpr uint32_t CLOEXEC       = 02000000;
constexpr size_t   MAX_INSTANCES = 64;
constexpr size_t   MAX_WATCHES   = 128;
constexpr size_t   MAX_EVENTS    = 64;
constexpr uint32_t IN_IGNORED    = 0x8000;
constexpr uint32_t IN_OVERFLOW   = 0x4000;
constexpr uint32_t IN_ONESHOT    = 0x80000000;
constexpr uint64_t AGAIN         = (uint64_t) -11;

struct notification_record_t
{
    int32_t  descriptor;
    uint32_t mask;
    uint32_t cookie;
    uint32_t length;
    char     name[256];
};
static_assert(offsetof(notification_record_t, name) == 16, "Linux inotify_event header");
struct signal_record_t
{
    uint32_t number;
    int32_t  error;
    int32_t  code;
    uint32_t pid;
    uint32_t uid;
    uint8_t  reserved[108];
};
static_assert(sizeof(signal_record_t) == 128, "Linux signalfd_siginfo ABI");
struct notification_watch_t
{
    inode_t* inode;
    uint32_t mask;
    int32_t  descriptor;
};
} // namespace

struct event_notification_t
{
    inode_t               inode;
    EventManager*         manager;
    file_t*               file;
    event_notification_t* next;
    uint32_t              kind;
};

namespace
{
struct inotify_state_t
{
    event_notification_t  common;
    notification_watch_t  watches[MAX_WATCHES];
    notification_record_t records[MAX_EVENTS];
    size_t                head;
    size_t                count;
    int32_t               nextDescriptor;
};
struct signal_descriptor_t
{
    event_notification_t common;
    uint64_t             mask;
};

void enqueue(inotify_state_t* state, const notification_record_t& record)
{
    if (state->count != 0)
    {
        notification_record_t& last = state->records[(state->head + state->count - 1) % MAX_EVENTS];
        if (memcmp(&last, &record, sizeof(record)) == 0)
        {
            return;
        }
    }
    if (state->count == MAX_EVENTS)
    {
        notification_record_t& last = state->records[(state->head + state->count - 1) % MAX_EVENTS];
        last                        = {};
        last.descriptor             = -1;
        last.mask                   = IN_OVERFLOW;
        return;
    }
    state->records[(state->head + state->count) % MAX_EVENTS] = record;
    ++state->count;
}

uint64_t pending(process_t* process, task_t* task)
{
    return __atomic_load_n(&process->signalPending, __ATOMIC_ACQUIRE) | __atomic_load_n(&task->signalPending, __ATOMIC_ACQUIRE);
}

bool copy_record(const user_memory_access_t* access, const file_iovec_t* vectors, size_t count, size_t offset, const void* record, size_t length)
{
    size_t copied = 0;
    for (size_t i = 0; i < count && copied < length; ++i)
    {
        if (offset >= vectors[i].length)
        {
            offset -= vectors[i].length;
            continue;
        }
        const size_t bytes = vectors[i].length - offset < length - copied ? vectors[i].length - offset : length - copied;
        if (vectors[i].address > UINT64_MAX - offset || !safe_write_user_locked(access, vectors[i].address + offset, static_cast<const uint8_t*>(record) + copied, bytes))
        {
            return false;
        }
        copied += bytes;
        offset = 0;
    }
    return copied == length;
}
} // namespace

const file_operations_t EventManager::InotifyOperations = {
        nullptr, nullptr, nullptr, NotificationSeek, nullptr, NotificationIoctl, nullptr, NotificationRelease, NotificationPoll, NotificationRead, NotificationWrite, NotificationReadVector, NotificationWriteVector,
};
const file_operations_t EventManager::SignalOperations = {
        nullptr, nullptr, nullptr, NotificationSeek, nullptr, nullptr, nullptr, NotificationRelease, NotificationPoll, NotificationRead, NotificationWrite, NotificationReadVector, NotificationWriteVector,
};

uint64_t EventManager::InstallNotification(event_notification_t* notification, const file_operations_t* operations, uint32_t flags)
{
    file_t* file = static_cast<file_t*>(kzalloc(sizeof(file_t)));
    if (file == nullptr)
    {
        kfree(notification);
        return LINUX_ENOMEM;
    }
    notification->manager = this;
    notification->file    = file;
    Vfs->InitializeAnonymousInode(&notification->inode, INODE_REGULAR, 0600);
    file->inode       = &notification->inode;
    file->operations  = operations;
    file->privateData = notification;
    file->statusFlags = flags & NONBLOCK;
    file->refCount    = 1;
    spinlock_acquire_preempt(&NotificationLock);
    size_t count = 0;
    for (auto item = Notifications; item != nullptr; item = item->next)
    {
        ++count;
    }
    if (count >= MAX_INSTANCES)
    {
        spinlock_release_preempt(&NotificationLock);
        kfree(file);
        kfree(notification);
        return (uint64_t) -23;
    }
    notification->next = Notifications;
    Notifications      = notification;
    spinlock_release_preempt(&NotificationLock);
    int64_t fd;
    {
        FileDescriptorGuard guard(Resources->processManager->GetCurrentProcess());
        process_t*          process = Resources->processManager->GetCurrentProcess();
        fd                          = process != nullptr ? Resources->processManager->AddFileDescriptor(process, file, flags & CLOEXEC ? FD_FLAG_CLOEXEC : 0, VirtualFileSystem::ReleaseFileHandle) : (int64_t) LINUX_ESRCH;
    }
    if (fd < 0)
    {
        Vfs->Close(file);
    }
    return (uint64_t) fd;
}

uint64_t EventManager::CreateInotify(uint32_t flags)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
    {
        return LINUX_ENOSYS;
    }
    if ((flags & ~(NONBLOCK | CLOEXEC)) != 0)
    {
        return LINUX_EINVAL;
    }
    inotify_state_t* state = static_cast<inotify_state_t*>(kzalloc(sizeof(inotify_state_t)));
    if (state == nullptr)
    {
        return LINUX_ENOMEM;
    }
    state->common.kind = 1;
    return InstallNotification(&state->common, &InotifyOperations, flags);
}

uint64_t EventManager::AddWatch(int32_t fd, uint64_t pointer, uint32_t mask)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
    {
        return LINUX_ENOSYS;
    }
    file_t* file = RetainDescriptor(fd);
    if (file == nullptr)
    {
        return LINUX_EBADF;
    }
    uint64_t result    = 0;
    inode_t* inode     = nullptr;
    char     path[512] = {};
    if (file->operations != &InotifyOperations)
    {
        result = LINUX_EINVAL;
    }
    else if ((mask & ~0xb7000fffU) != 0 || (mask & 0xfff) == 0 || (mask & 0x30000000U) == 0x30000000U)
    {
        result = LINUX_EINVAL;
    }
    else if (mask & 0x04000000U)
    {
        result = LINUX_EOPNOTSUPP;
    }
    else if (!safe_read_user_string(pointer, path, sizeof(path), nullptr))
    {
        result = LINUX_EFAULT;
    }
    else
    {
        char       absolute[512];
        process_t* process = Resources->processManager->GetCurrentProcess();
        if (process == nullptr)
        {
            result = LINUX_ESRCH;
        }
        else if (path[0] == 0)
        {
            result = LINUX_ENOENT;
        }
        else if (!Vfs->JoinPath(process->cwdPath, path, absolute, sizeof(absolute)))
        {
            result = LINUX_ENAMETOOLONG;
        }
        else if (mask & 0x02000000)
        {
            result = (uint64_t) Vfs->StatNoFollow({}, absolute, &inode);
        }
        else
        {
            vfs_path_t resolved = {};
            if (!Vfs->ResolvePath({}, absolute, &resolved) || resolved.dentry == nullptr)
            {
                result = LINUX_ENOENT;
            }
            else
            {
                inode = resolved.dentry->inode;
            }
        }
        if (result == 0 && (mask & 0x01000000U) && inode->type != INODE_DIRECTORY)
        {
            result = LINUX_ENOTDIR;
        }
    }
    if (result == 0)
    {
        auto state = static_cast<inotify_state_t*>(file->privateData);
        spinlock_acquire_preempt(&NotificationLock);
        notification_watch_t* slot = nullptr;
        for (auto& watch : state->watches)
        {
            if (watch.inode == nullptr && slot == nullptr)
            {
                slot = &watch;
            }
            if (watch.inode == inode)
            {
                slot = &watch;
                break;
            }
        }
        if (slot == nullptr || state->nextDescriptor == INT32_MAX)
        {
            result = (uint64_t) -28;
        }
        else if (slot->inode != nullptr && (mask & 0x10000000U))
        {
            result = (uint64_t) -17;
        }
        else
        {
            if (slot->inode == nullptr)
            {
                slot->inode      = inode;
                slot->descriptor = ++state->nextDescriptor;
                slot->mask       = 0;
            }
            slot->mask = (mask & 0x20000000U ? slot->mask : 0U) | (mask & (0xfffU | IN_ONESHOT));
            result     = (uint64_t) slot->descriptor;
        }
        spinlock_release_preempt(&NotificationLock);
    }
    Vfs->Close(file);
    return result;
}

uint64_t EventManager::RemoveWatch(int32_t fd, int32_t descriptor)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
    {
        return LINUX_ENOSYS;
    }
    file_t* file = RetainDescriptor(fd);
    if (file == nullptr)
    {
        return LINUX_EBADF;
    }
    uint64_t result = LINUX_EINVAL;
    if (file->operations == &InotifyOperations)
    {
        spinlock_acquire_preempt(&NotificationLock);
        auto state = static_cast<inotify_state_t*>(file->privateData);
        for (auto& watch : state->watches)
        {
            if (watch.inode != nullptr && watch.descriptor == descriptor)
            {
                watch                        = {};
                notification_record_t record = {};
                record.descriptor            = descriptor;
                record.mask                  = IN_IGNORED;
                enqueue(state, record);
                result = 0;
                break;
            }
        }
        spinlock_release_preempt(&NotificationLock);
        if (result == 0)
        {
            NotifySource(file, EVENT_POLL_IN);
        }
    }
    Vfs->Close(file);
    return result;
}

void EventManager::NotifyVfs(inode_t* inode, uint32_t mask, inode_t* parent, const char* name)
{
    file_t* changed[MAX_INSTANCES];
    size_t  count = 0;
    spinlock_acquire_preempt(&NotificationLock);
    for (auto notification = Notifications; notification != nullptr; notification = notification->next)
    {
        if (notification->kind != 1)
        {
            continue;
        }
        auto state = reinterpret_cast<inotify_state_t*>(notification);
        bool ready = false;
        for (auto& watch : state->watches)
        {
            if (watch.inode == nullptr || (watch.mask & mask & 0xfff) == 0 || (watch.inode != inode && watch.inode != parent))
            {
                continue;
            }
            notification_record_t record = {};
            record.descriptor            = watch.descriptor;
            record.mask                  = mask | (inode != nullptr && inode->type == INODE_DIRECTORY ? 0x40000000U : 0U);
            if (watch.inode == parent && name != nullptr)
            {
                size_t length = strlen(name);
                if (length > 255)
                {
                    length = 255;
                }
                memcpy(record.name, name, length);
                record.length = (uint32_t) ((length + 1 + 15) & ~15ULL);
            }
            enqueue(state, record);
            ready = true;
            if (watch.mask & IN_ONESHOT)
            {
                watch         = {};
                record.mask   = IN_IGNORED;
                record.length = 0;
                memset(record.name, 0, sizeof(record.name));
                enqueue(state, record);
            }
        }
        if (ready && Vfs->Retain(notification->file))
        {
            changed[count++] = notification->file;
        }
    }
    spinlock_release_preempt(&NotificationLock);
    for (size_t i = 0; i < count; ++i)
    {
        NotifySource(changed[i], EVENT_POLL_IN);
        Vfs->Close(changed[i]);
    }
}

uint64_t EventManager::SignalDescriptor(int32_t fd, uint64_t mask, uint32_t flags)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr)
    {
        return LINUX_ENOSYS;
    }
    if ((flags & ~(NONBLOCK | CLOEXEC)) != 0)
    {
        return LINUX_EINVAL;
    }
    mask &= ~SIGNAL_UNBLOCKABLE;
    if (mask & ~0x7fffffffULL)
    {
        return LINUX_EOPNOTSUPP;
    }
    if (fd == -1)
    {
        auto state = static_cast<signal_descriptor_t*>(kzalloc(sizeof(signal_descriptor_t)));
        if (state == nullptr)
        {
            return LINUX_ENOMEM;
        }
        state->common.kind = 2;
        state->mask        = mask;
        return InstallNotification(&state->common, &SignalOperations, flags);
    }
    file_t* file = RetainDescriptor(fd);
    if (file == nullptr)
    {
        return LINUX_EBADF;
    }
    uint64_t result = LINUX_EINVAL;
    if (file->operations == &SignalOperations)
    {
        auto state = static_cast<signal_descriptor_t*>(file->privateData);
        __atomic_store_n(&state->mask, mask, __ATOMIC_RELEASE);
        NotifySource(file, NotificationPoll(file));
        result = (uint32_t) fd;
    }
    Vfs->Close(file);
    return result;
}

void EventManager::NotifySignals(process_t* process)
{
    if (Resources == nullptr || Resources->taskManager == nullptr || process == nullptr)
    {
        return;
    }
    uint64_t     bits  = __atomic_load_n(&process->signalPending, __ATOMIC_ACQUIRE);
    TaskManager* tasks = Resources->taskManager;
    for (size_t i = 0; i < TaskManager::MAX_TASKS; ++i)
    {
        task_t* task = &tasks->GetTasks()[i];
        if (tasks->RetainForDispatch(task))
        {
            if (task->process == process || (task == tasks->GetCurrentTask() && Resources->processManager->GetCurrentProcess() == process))
            {
                bits |= __atomic_load_n(&task->signalPending, __ATOMIC_ACQUIRE);
            }
            tasks->ReleaseDispatchClaim(task);
        }
    }
    file_t* changed[MAX_INSTANCES];
    size_t  count = 0;
    spinlock_acquire_preempt(&NotificationLock);
    for (auto notification = Notifications; notification != nullptr; notification = notification->next)
    {
        if (notification->kind == 2 && (__atomic_load_n(&reinterpret_cast<signal_descriptor_t*>(notification)->mask, __ATOMIC_ACQUIRE) & bits) != 0 && Vfs->Retain(notification->file))
        {
            changed[count++] = notification->file;
        }
    }
    spinlock_release_preempt(&NotificationLock);
    for (size_t i = 0; i < count; ++i)
    {
        NotifySource(changed[i], EVENT_POLL_IN);
        Vfs->Close(changed[i]);
    }
}

void EventManager::NotificationRelease(file_t* file)
{
    auto notification = static_cast<event_notification_t*>(file->privateData);
    auto manager      = notification->manager;
    spinlock_acquire_preempt(&manager->NotificationLock);
    auto link = &manager->Notifications;
    while (*link != notification)
    {
        link = &(*link)->next;
    }
    *link = notification->next;
    spinlock_release_preempt(&manager->NotificationLock);
    kfree(notification);
}

uint32_t EventManager::NotificationPoll(file_t* file)
{
    auto notification = static_cast<event_notification_t*>(file->privateData);
    auto manager      = notification->manager;
    if (notification->kind == 1)
    {
        spinlock_acquire_preempt(&manager->NotificationLock);
        const bool ready = reinterpret_cast<inotify_state_t*>(notification)->count != 0;
        spinlock_release_preempt(&manager->NotificationLock);
        return ready ? EVENT_POLL_IN : 0;
    }
    process_t* process = manager->Resources->processManager->GetCurrentProcess();
    task_t*    task    = manager->Resources->taskManager->GetCurrentTask();
    if (process == nullptr || task == nullptr)
    {
        return 0;
    }
    const uint64_t mask = __atomic_load_n(&reinterpret_cast<signal_descriptor_t*>(notification)->mask, __ATOMIC_ACQUIRE);
    return (pending(process, task) & mask) != 0 ? EVENT_POLL_IN : 0;
}

int64_t EventManager::NotificationSeek(file_t*, int64_t, int)
{
    return -29;
}

int64_t EventManager::NotificationIoctl(file_t* file, uint64_t request, uint64_t pointer)
{
    if (request != 0x541b)
    {
        return (int64_t) LINUX_ENOTTY;
    }
    user_memory_access_t access;
    if (!begin_user_memory(&access))
    {
        return (int64_t) LINUX_ESRCH;
    }
    auto notification = static_cast<event_notification_t*>(file->privateData);
    auto manager      = notification->manager;
    auto state        = reinterpret_cast<inotify_state_t*>(notification);
    spinlock_acquire_preempt(&manager->NotificationLock);
    int32_t size = 0;
    for (size_t i = 0; i < state->count; ++i)
    {
        size += 16 + state->records[(state->head + i) % MAX_EVENTS].length;
    }
    const bool copied = safe_write_user_locked(&access, pointer, &size, sizeof(size));
    spinlock_release_preempt(&manager->NotificationLock);
    end_user_memory(&access);
    return copied ? 0 : (int64_t) LINUX_EFAULT;
}

uint64_t EventManager::NotificationRead(file_t* file, uint64_t address, uint64_t count)
{
    const file_iovec_t vector = {address, count};
    return NotificationReadVector(file, &vector, 1);
}

uint64_t EventManager::NotificationReadVector(file_t* file, const file_iovec_t* vectors, size_t count)
{
    size_t capacity = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (vectors[i].length > INT64_MAX - capacity)
        {
            return LINUX_EINVAL;
        }
        capacity += vectors[i].length;
    }
    auto notification = static_cast<event_notification_t*>(file->privateData);
    auto manager      = notification->manager;
    if (capacity < (notification->kind == 1 ? 16 : 128))
    {
        return LINUX_EINVAL;
    }
    for (;;)
    {
        const uint64_t       sequence = manager->Sequence();
        user_memory_access_t access;
        if (!begin_user_memory(&access))
        {
            return LINUX_ESRCH;
        }
        uint64_t result = AGAIN;
        size_t   copied = 0;
        bool     alarm  = false;
        if (notification->kind == 1)
        {
            auto state = reinterpret_cast<inotify_state_t*>(notification);
            spinlock_acquire_preempt(&manager->NotificationLock);
            while (state->count != 0)
            {
                auto&        record = state->records[state->head];
                const size_t size   = 16 + record.length;
                if (size > capacity - copied)
                {
                    result = LINUX_EINVAL;
                    break;
                }
                if (!copy_record(&access, vectors, count, copied, &record, size))
                {
                    result = LINUX_EFAULT;
                    break;
                }
                copied += size;
                state->head = (state->head + 1) % MAX_EVENTS;
                --state->count;
            }
            spinlock_release_preempt(&manager->NotificationLock);
        }
        else
        {
            process_t* process = manager->Resources->processManager->GetCurrentProcess();
            task_t*    task    = manager->Resources->taskManager->GetCurrentTask();
            if (process == nullptr || task == nullptr)
            {
                result = LINUX_ESRCH;
            }
            else
            {
                const uint64_t mask = __atomic_load_n(&reinterpret_cast<signal_descriptor_t*>(notification)->mask, __ATOMIC_ACQUIRE);
                spinlock_acquire_preempt(&process->signalLock);
                while (capacity - copied >= sizeof(signal_record_t))
                {
                    const uint64_t available = pending(process, task) & mask;
                    if (available == 0)
                    {
                        break;
                    }
                    const uint32_t  index  = (uint32_t) __builtin_ctzll(available);
                    const uint64_t  bit    = 1ULL << index;
                    const bool      thread = (__atomic_load_n(&task->signalPending, __ATOMIC_ACQUIRE) & bit) != 0;
                    signal_sender_t sender = thread && task->signalSenders != nullptr ? task->signalSenders[index] : process->signalSenders[index];
                    signal_record_t record = {};
                    record.number          = index + 1;
                    record.pid             = sender.pid;
                    record.uid             = sender.uid;
                    record.code            = sender.code;
                    if (!copy_record(&access, vectors, count, copied, &record, sizeof(record)))
                    {
                        result = LINUX_EFAULT;
                        break;
                    }
                    if (thread)
                    {
                        __atomic_fetch_and(&task->signalPending, ~bit, __ATOMIC_RELEASE);
                    }
                    else
                    {
                        __atomic_fetch_and(&process->signalPending, ~bit, __ATOMIC_RELEASE);
                    }
                    copied += sizeof(record);
                    alarm |= index == 13;
                }
                spinlock_release_preempt(&process->signalLock);
                if (alarm && manager->Resources->timeManager != nullptr)
                {
                    manager->Resources->timeManager->RestartRealTimer(process);
                }
            }
        }
        end_user_memory(&access);
        if (copied != 0)
        {
            manager->Notify();
            return copied;
        }
        if (result != AGAIN || (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & NONBLOCK))
        {
            return result;
        }
        result = manager->Wait(sequence);
        if (result != 0)
        {
            return result;
        }
    }
}

uint64_t EventManager::NotificationWrite(file_t*, uint64_t, uint64_t)
{
    return LINUX_EINVAL;
}

uint64_t EventManager::NotificationWriteVector(file_t*, const file_iovec_t*, size_t)
{
    return LINUX_EINVAL;
}
