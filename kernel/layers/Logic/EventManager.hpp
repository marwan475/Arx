#pragma once

#include <stddef.h>
#include <stdint.h>
#include <klib/spinlock.h>

struct ResourceLayerCaps;
struct file_t;
struct event_epoll_t;
class Scheduler;
class VirtualFileSystem;

struct event_pollfd_t
{
    int32_t fd;
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
    EVENT_POLL_IN = 0x001,
    EVENT_POLL_PRI = 0x002,
    EVENT_POLL_OUT = 0x004,
    EVENT_POLL_ERR = 0x008,
    EVENT_POLL_HUP = 0x010,
    EVENT_POLL_NVAL = 0x020,
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
    uint64_t CreateEpoll(uint32_t flags);
    uint64_t ControlEpoll(int32_t epfd, int32_t operation, int32_t fd, const event_epoll_event_t* event);
    uint64_t WaitEpoll(int32_t epfd, event_epoll_event_t* events, size_t count, uint64_t deadline);
    uint64_t Poll(event_pollfd_t* descriptors, size_t count, uint64_t deadline);
    void Notify();
    uint64_t Sequence() const;
    uint64_t Wait(uint64_t expected, uint64_t deadline = UINT64_MAX);

private:
    file_t* RetainDescriptor(int32_t fd);
    static void FileClosing(void* context, file_t* file);
    static void EpollRelease(file_t* file);
    static uint32_t EpollPoll(file_t* file);
    static int64_t EpollSeek(file_t* file, int64_t offset, int whence);
    uint64_t ScanEpoll(event_epoll_t* epoll, event_epoll_event_t* events, size_t count);
    static uint32_t CounterPoll(file_t* file);
    static int64_t CounterSeek(file_t* file, int64_t offset, int whence);
    static uint64_t CounterRead(file_t* file, uint64_t address, uint64_t count);
    static uint64_t CounterWrite(file_t* file, uint64_t address, uint64_t count);
    static void CounterRelease(file_t* file);
    ResourceLayerCaps* Resources;
    Scheduler* SchedulerManager;
    VirtualFileSystem* Vfs;
    uint64_t Generation;
    spinlock_t EpollLock;
    event_epoll_t* Epolls;
};
