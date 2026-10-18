#pragma once

#include "layers/DispatcherRoutes.hpp"

#include <klib/spinlock.h>
#include <stddef.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct virt_addr_space;
struct task_t;
struct process_t;
struct virt_backing_object;
class  Scheduler;

class SynchronizationManager
{
public:
    SynchronizationManager(ResourceLayerCaps* resources, Scheduler* scheduler);
    uint64_t Wait(uint64_t address, uint32_t expected, uint32_t bits, uint64_t deadline, bool shared = false, uint64_t priorityDestination = 0);
    uint64_t Wake(uint64_t address, int32_t count, uint32_t bits, bool shared                                                                   = false);
    uint64_t Requeue(uint64_t address, uint64_t destination, int32_t wakeCount, int32_t moveCount, bool compare, uint32_t expected, bool shared = false);
    uint64_t WakeOperation(uint64_t address, uint64_t destination, int32_t wakeCount, int32_t secondCount, uint32_t operation, bool shared      = false);
    uint64_t PriorityFutex(uint64_t address, uint32_t command, uint64_t deadline, bool shared);
    uint64_t RequeuePriority(uint64_t address, uint64_t destination, int32_t wakeCount, int32_t moveCount, uint32_t expected, bool shared);
    bool LockSchedulingChange(task_t* task, uint64_t affinity = 0);
    void UnlockSchedulingChange();
    void     RefreshPriorities();
    uint64_t SetRobustList(uint64_t head);
    void     ConfigureChildTid(task_t* task, process_t* owner, uint64_t address);
    uint64_t GetRobustList(int32_t tid, uint64_t* head, uint64_t* length);

private:
    static constexpr size_t CAPACITY = 128;
    static constexpr size_t NONE     = (size_t) -1;
    struct waiter_t
    {
        void*                identity; // Address space for private keys, backing for shared.
        virt_backing_object* object; // Pins shared identity through wake/requeue/unmap.
        uint64_t             offset;
        uint64_t             sequence;
        uint32_t             bits;
        size_t               next;
        bool                 used;
        bool                 queued;
        bool                 woken;
        bool                 requeuePriority;
        bool                 converted;
        void*                destinationIdentity;
        virt_backing_object* destinationObject;
        uint64_t             destinationOffset;
        task_t*              task;
    };
    struct pi_state_t
    {
        bool                 used;
        bool                 dead;
        void*                identity;
        virt_backing_object* object;
        uint64_t             offset;
        task_t*              owner;
        process_t*           memoryOwner;
        uint64_t             address;
    };
    struct pi_waiter_t
    {
        bool     used;
        size_t   state;
        task_t*  task;
        uint64_t sequence;
        uint64_t order;
    };
    pi_state_t         PiStates[CAPACITY];
    pi_waiter_t        PiWaiters[CAPACITY];
    uint64_t           PiOrder;
    uint64_t           PriorityRescheduleMask;
    bool               IsRequeueKeyLocked(void* identity, uint64_t offset) const;
    bool               IsPriorityKeyLocked(void* identity, uint64_t offset) const;
    bool               HasPiWaiters(size_t state) const;
    size_t             BestPiWaiter(size_t state) const;
    void               RefreshPrioritiesLocked();
    static void        CleanupPriority(void* context, task_t* task);
    void               Remove(size_t index);
    uint64_t           WakeKey(void* identity, uint64_t offset, int32_t count, uint32_t bits);
    void               WakeExitWord(virt_addr_space* space, uint64_t address);
    size_t             WakeKeyLocked(void* identity, uint64_t offset, uint32_t count, uint32_t bits, const uint64_t** notify);
    void               NotifyWaiters(const uint64_t** notify, size_t count);
    static void        CleanupRobust(void* context, task_t* task);
    const dispatcher_task_cleanup_t PriorityCleanupRoute;
    const dispatcher_task_cleanup_t RobustCleanupRoute;

    ResourceLayerCaps* Resources;
    Scheduler*         SchedulerManager;
    spinlock_t         Lock;
    waiter_t           Waiters[CAPACITY];
    size_t             Head;
    size_t             Tail;
};
