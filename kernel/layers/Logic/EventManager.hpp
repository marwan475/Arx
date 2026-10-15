#pragma once

#include <klib/spinlock.h>
#include <stddef.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct file_t;
struct event_epoll_t;
struct event_epoll_watch_t;
struct file_iovec_t;
struct user_memory_access;
class Scheduler;
class VirtualFileSystem;
struct inode_t;
struct process_t;
struct event_notification_t;

struct event_pollfd_t
{
    int32_t  fd;
    uint16_t events;
    uint16_t revents;
};
static_assert(sizeof(event_pollfd_t) == 8, "Linux pollfd ABI");
static_assert(offsetof(event_pollfd_t, revents) == 6, "Linux pollfd result offset");

struct __attribute__((packed)) event_epoll_event_t
{
    uint32_t events;
    uint64_t data;
};
static_assert(sizeof(event_epoll_event_t) == 12, "Linux x86-64 epoll_event ABI");
static_assert(offsetof(event_epoll_event_t, data) == 4, "Linux epoll data offset");

enum : uint16_t
{
    EVENT_POLL_IN     = 0x001,
    EVENT_POLL_PRI    = 0x002,
    EVENT_POLL_OUT    = 0x004,
    EVENT_POLL_ERR    = 0x008,
    EVENT_POLL_HUP    = 0x010,
    EVENT_POLL_NVAL   = 0x020,
    EVENT_POLL_RDNORM = 0x040,
    EVENT_POLL_RDBAND = 0x080,
    EVENT_POLL_WRNORM = 0x100,
    EVENT_POLL_WRBAND = 0x200,
};

class EventManager
{
public:
    EventManager(ResourceLayerCaps* resources, Scheduler* scheduler, VirtualFileSystem* vfs);
    uint64_t CreateCounter(uint32_t initial, uint32_t flags);
    uint64_t CreateInotify(uint32_t flags);
    uint64_t AddWatch(int32_t fd, uint64_t path, uint32_t mask);
    uint64_t RemoveWatch(int32_t fd, int32_t watch);
    uint64_t SignalDescriptor(int32_t fd, uint64_t mask, uint32_t flags);
    void     NotifyVfs(inode_t* inode, uint32_t mask, inode_t* parent = nullptr, const char* name = nullptr, uint32_t cookie = 0);
    void     NotifySignals(process_t* process);
    uint64_t CreateEpoll(uint32_t flags);
    uint64_t ControlEpoll(int32_t epfd, int32_t operation, int32_t fd, const event_epoll_event_t* event);
    uint64_t WaitEpoll(int32_t epfd, uint64_t address, size_t count, uint64_t deadline);
    uint64_t Poll(event_pollfd_t* descriptors, size_t count, uint64_t deadline);
    void     Notify();
    // Producers publish state first, then notify outside their state locks.
    // Edge-triggered watches require the source-specific notification path.
    void     NotifySource(file_t* file, uint32_t events);
    uint64_t Sequence() const;
    uint64_t Wait(uint64_t expected, uint64_t deadline = UINT64_MAX);

private:
    static const struct file_operations_t InotifyOperations;
    static const struct file_operations_t SignalOperations;
    uint64_t                              InstallNotification(event_notification_t* notification, const struct file_operations_t* operations, uint32_t flags);
    static void                           NotificationRelease(file_t* file);
    static uint32_t                       NotificationPoll(file_t* file);
    static uint64_t                       NotificationRead(file_t* file, uint64_t address, uint64_t count);
    static uint64_t                       NotificationWrite(file_t* file, uint64_t address, uint64_t count);
    static uint64_t                       NotificationWriteVector(file_t* file, const file_iovec_t* vectors, size_t count);
    static uint64_t                       NotificationReadVector(file_t* file, const file_iovec_t* vectors, size_t count);
    static int64_t                        NotificationSeek(file_t* file, int64_t offset, int whence);
    static int64_t                        NotificationIoctl(file_t* file, uint64_t request, uint64_t argument);
    spinlock_t                            NotificationLock;
    event_notification_t*                 Notifications;
    file_t*                               RetainDescriptor(int32_t fd);
    static void                           FileClosing(void* context, file_t* file);
    static void                           EpollRelease(file_t* file);
    static uint32_t                       EpollPoll(file_t* file);
    static int64_t                        EpollSeek(file_t* file, int64_t offset, int whence);
    bool                                  CheckEpollDepth(event_epoll_t* epoll, event_epoll_t* owner, event_epoll_t* target, size_t depth);
    void                                  NotifySourceLocked(file_t* file, uint32_t events, size_t depth);
    uint32_t                              WatchReadiness(event_epoll_watch_t* watch);
    uint64_t                              ScanEpoll(event_epoll_t* epoll, const user_memory_access* access, uint64_t address, size_t count);
    static uint32_t                       CounterPoll(file_t* file);
    static int64_t                        CounterSeek(file_t* file, int64_t offset, int whence);
    static uint64_t                       CounterRead(file_t* file, uint64_t address, uint64_t count);
    static uint64_t                       CounterWrite(file_t* file, uint64_t address, uint64_t count);
    static uint64_t                       CounterReadVector(file_t* file, const file_iovec_t* vectors, size_t count);
    static uint64_t                       CounterWriteVector(file_t* file, const file_iovec_t* vectors, size_t count);
    static void                           CounterRelease(file_t* file);
    ResourceLayerCaps*                    Resources;
    Scheduler*                            SchedulerManager;
    VirtualFileSystem*                    Vfs;
    uint64_t                              Generation;
    uint64_t                              TopologyGeneration;
    uint64_t                              NotificationGeneration;
    spinlock_t                            EpollLock;
    event_epoll_t*                        Epolls;
};
