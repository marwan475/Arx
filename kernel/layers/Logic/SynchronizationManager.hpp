#pragma once

#include <klib/spinlock.h>
#include <stddef.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct virt_addr_space;
struct task_t;
class Scheduler;

class SynchronizationManager
{
public:
    SynchronizationManager(ResourceLayerCaps* resources, Scheduler* scheduler);
    uint64_t Wait(uint64_t address, uint32_t expected, uint32_t bits, uint64_t deadline);
    uint64_t Wake(uint64_t address, int32_t count, uint32_t bits);
    uint64_t Requeue(uint64_t address, uint64_t destination, int32_t wakeCount, int32_t moveCount, bool compare, uint32_t expected);
    uint64_t WakeOperation(uint64_t address, uint64_t destination, int32_t wakeCount, int32_t secondCount, uint32_t operation);
    uint64_t SetRobustList(uint64_t head);
    uint64_t GetRobustList(int32_t tid, uint64_t* head, uint64_t* length);

private:
    static constexpr size_t CAPACITY = 128;
    static constexpr size_t NONE     = (size_t) -1;
    struct waiter_t
    {
        virt_addr_space* space;
        uint64_t         address;
        uint64_t         sequence;
        uint32_t         bits;
        size_t           next;
        bool             used;
        bool             queued;
        bool             woken;
    };
    void               Remove(size_t index);
    uint64_t           WakeKey(virt_addr_space* space, uint64_t address, int32_t count, uint32_t bits);
    size_t             WakeKeyLocked(virt_addr_space* space, uint64_t address, uint32_t count, uint32_t bits, const uint64_t** notify);
    void               NotifyWaiters(const uint64_t** notify, size_t count);
    static void        CleanupRobust(void* context, task_t* task);
    ResourceLayerCaps* Resources;
    Scheduler*         SchedulerManager;
    spinlock_t         Lock;
    waiter_t           Waiters[CAPACITY];
    size_t             Head;
    size_t             Tail;
};
