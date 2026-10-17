#include "layers/Logic/SynchronizationManager.hpp"

#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <klib/klib.h>

namespace
{
struct FutexKey
{
    void*                  identity = nullptr;
    uint64_t               offset   = 0;
    virt_backing_object_t* object   = nullptr;
    ~FutexKey()
    {
        VirtualMemoryManager::ReleaseBacking(object);
    }

    uint64_t Resolve(VirtualMemoryManager* memory, virt_addr_space_t* space, uint64_t address, bool shared)
    {
        identity = space;
        offset   = address;

        if (!shared)
        {
            return 0;
        }

        if (memory == nullptr)
        {
            return LINUX_ENOSYS;
        }

        auto* region = memory->FindRegion(space, address);

        if (region == nullptr || region->backing == nullptr || region->protection == 0)
        {
            return LINUX_EFAULT;
        }

        if (!region->shared_mapping || region->object == nullptr)
        {
            return LINUX_EOPNOTSUPP;
        }

        object = region->object;
        VirtualMemoryManager::RetainBacking(object);
        identity = object;
        offset   = region->backing_offset + address - region->start;

        return 0;
    }
};
}

SynchronizationManager::SynchronizationManager(ResourceLayerCaps* resources, Scheduler* scheduler) : Resources(resources), SchedulerManager(scheduler), Lock(0), Waiters{}, Head(NONE), Tail(NONE)
{
    memset(PiStates, 0, sizeof(PiStates));
    memset(PiWaiters, 0, sizeof(PiWaiters));
    PiOrder = 0;
    PriorityRescheduleMask = 0;
    if (Resources != nullptr && Resources->taskManager != nullptr)
    {
        Resources->taskManager->SetPriorityCleanup(this, CleanupPriority);
    }
    static_assert(CAPACITY == TaskManager::MAX_TASKS, "one futex waiter per task");
}

void SynchronizationManager::Remove(size_t index)
{
    size_t* link     = &Head;
    size_t  previous = NONE;

    while (*link != NONE && *link != index)
    {
        previous = *link;
        link     = &Waiters[*link].next;
    }

    if (*link == NONE)
    {
        return;
    }

    *link = Waiters[index].next;

    if (Tail == index)
    {
        Tail = previous;
    }

    Waiters[index].queued = false;
}

uint64_t SynchronizationManager::Wait(uint64_t address, uint32_t expected, uint32_t bits, uint64_t deadline, bool shared, uint64_t priorityDestination)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Resources->taskManager == nullptr || SchedulerManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if ((address & 3) != 0 || bits == 0)
    {
        return LINUX_EINVAL;
    }

    ProcessManager* processes = Resources->processManager;
    process_t*      process   = processes->AcquireCurrentMemoryProcess(!shared);

    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    task_t*  task   = Resources->taskManager->GetCurrentTask();
    uint64_t result = [&]() -> uint64_t
    {
        if (task == nullptr || task->id >= CAPACITY)
        {
            return LINUX_ESRCH;
        }

        user_memory_access_t access;

        if (!begin_user_memory(&access))
        {
            return LINUX_ESRCH;
        }

        const size_t index  = (size_t) task->id;
        waiter_t&    waiter = Waiters[index];
        FutexKey key, destinationKey;
        uint64_t keyStatus = key.Resolve(Resources->virtualMemoryManager, access.space, address, shared);
        if (keyStatus == 0 && priorityDestination != 0)
        {
            if ((priorityDestination & 3) != 0)
            {
                keyStatus = LINUX_EINVAL;
            }
            else
            {
                keyStatus = destinationKey.Resolve(Resources->virtualMemoryManager, access.space, priorityDestination, shared);
                if (keyStatus == 0 && key.identity == destinationKey.identity && key.offset == destinationKey.offset)
                {
                    keyStatus = LINUX_EINVAL;
                }
            }
        }
        spinlock_acquire_preempt(&Lock);
        uint32_t actual;
        uint64_t status = 0;

        if (keyStatus != 0)
        {
            status = keyStatus;
        }
        else if (IsPriorityKeyLocked(key.identity, key.offset))
        {
            status = LINUX_EINVAL;
        }
        else if (waiter.used)
        {
            status = LINUX_EINVAL;
        }
        else if (!safe_atomic_load_user32_locked(&access, address, &actual))
            status = LINUX_EFAULT;
        else if (actual != expected)
            status = (uint64_t) -11; // EAGAIN

        if (status == 0)
        {
            waiter.identity = key.identity;
            waiter.offset   = key.offset;
            waiter.object   = key.object;
            VirtualMemoryManager::RetainBacking(waiter.object);
            waiter.bits    = bits;
            waiter.used = waiter.queued = true;
            waiter.woken                = false;
            waiter.requeuePriority = priorityDestination != 0;
            waiter.converted = false;
            waiter.destinationIdentity = destinationKey.identity;
            waiter.destinationOffset = destinationKey.offset;
            waiter.destinationObject = destinationKey.object;
            waiter.task = task;
            VirtualMemoryManager::RetainBacking(waiter.destinationObject);
            waiter.next                 = NONE;

            if (Tail != NONE)
            {
                Waiters[Tail].next = index;
            }
            else
            {
                Head = index;
            }

            Tail = index;
        }

        const uint64_t sequence = __atomic_load_n(&waiter.sequence, __ATOMIC_ACQUIRE);
        spinlock_release_preempt(&Lock);
        end_user_memory(&access);

        if (status != 0)
        {
            return status;
        }

        for (;;)
        {
            uint64_t   now   = 0;
            const bool clock = deadline == UINT64_MAX || arch_time_read_monotonic(&now);
            spinlock_acquire_preempt(&Lock);

            if (waiter.woken)
            {
                status = 0;
            }
            else if (!clock)
                status = LINUX_ENOSYS;
            else if (deadline != UINT64_MAX && now >= deadline)
                status = (uint64_t) -110; // ETIMEDOUT
            else
            {
                spinlock_release_preempt(&Lock);
                status = SchedulerManager->WaitForEvent(&waiter.sequence, sequence, deadline);

                if (status == 0)
                {
                    continue;
                }

                spinlock_acquire_preempt(&Lock);
                // A concurrent wake already consumed this waiter; report that success.

                if (waiter.woken)
                {
                    status = 0;
                }
            }

            // Slot and process lifetime remain protected until after dequeue.

            if (waiter.queued)
            {
                Remove(index);
            }

            const bool converted = waiter.converted;
            auto* destinationObject = waiter.destinationObject;
            waiter.destinationObject = nullptr;
            waiter.used     = false;
            waiter.identity = nullptr;
            auto* object    = waiter.object;
            waiter.object   = nullptr;
            spinlock_release_preempt(&Lock);
            VirtualMemoryManager::ReleaseBacking(object);
            VirtualMemoryManager::ReleaseBacking(destinationObject);
            if (converted)
            {
                // Requeue has already installed the PI edge under Lock. Join
                // that edge before reporting success; cancellation removes it.
                return PriorityFutex(priorityDestination, 13, deadline, shared);
            }
            return status;
        }
    }();
    processes->ReleaseMemoryProcess(process);
    return result;
}

uint64_t SynchronizationManager::Wake(uint64_t address, int32_t count, uint32_t bits, bool shared)
{
    if (Resources == nullptr || Resources->processManager == nullptr || SchedulerManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if ((address & 3) != 0 || bits == 0)
    {
        return LINUX_EINVAL;
    }

    if (!range_within_u64(address, sizeof(uint32_t), CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return LINUX_EFAULT;
    }

    ProcessManager* processes = Resources->processManager;
    process_t*      process   = processes->AcquireCurrentMemoryProcess(!shared);

    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    uint64_t result;

    if (shared)
    {
        user_memory_access_t access;

        if (!begin_user_memory(&access))
        {
            result = LINUX_ESRCH;
        }
        else
        {
            FutexKey key;
            uint32_t value;
            result = key.Resolve(Resources->virtualMemoryManager, access.space, address, true);

            if (result == 0 && !safe_atomic_load_user32_locked(&access, address, &value))
            {
                result = LINUX_EFAULT;
            }

            if (result == 0)
            {
                result = WakeKey(key.identity, key.offset, count, bits);
            }

            end_user_memory(&access);
        }
    }
    else
    {
        result = WakeKey(process->addressSpace, address, count, bits);
    }
    processes->ReleaseMemoryProcess(process);
    return result;
}

uint64_t SynchronizationManager::WakeKey(void* identity, uint64_t offset, int32_t count, uint32_t bits)
{
    const uint64_t* notify[CAPACITY];
    // Legacy Linux FUTEX_WAKE/WAKE_BITSET wakes the first matching waiter even
    // for a nonpositive nr_wake (the limit is checked after removing it).
    const uint32_t limit = count > 0 ? (uint32_t) count : 1U;
    spinlock_acquire_preempt(&Lock);
    if (IsPriorityKeyLocked(identity, offset) || IsRequeueKeyLocked(identity, offset))
    {
        spinlock_release_preempt(&Lock);
        return LINUX_EINVAL;
    }
    const size_t woken = WakeKeyLocked(identity, offset, limit, bits, notify);
    spinlock_release_preempt(&Lock);
    NotifyWaiters(notify, woken);
    return woken;
}

size_t SynchronizationManager::WakeKeyLocked(void* identity, uint64_t offset, uint32_t count, uint32_t bits, const uint64_t** notify)
{
    size_t woken = 0;
    size_t index = Head;

    while (index != NONE && woken < count)
    {
        waiter_t&    waiter = Waiters[index];
        const size_t next   = waiter.next;

        if (waiter.identity == identity && waiter.offset == offset && (waiter.bits & bits) != 0)
        {
            Remove(index);
            waiter.woken = true;
            __atomic_add_fetch(&waiter.sequence, 1ULL, __ATOMIC_RELEASE);
            notify[woken++] = &waiter.sequence;
        }

        index = next;
    }

    return woken;
}

void SynchronizationManager::NotifyWaiters(const uint64_t** notify, size_t count)
{
    SchedulerManager->RequestReschedule(__atomic_exchange_n(&PriorityRescheduleMask, 0ULL, __ATOMIC_ACQ_REL));
    for (size_t i = 0; i < count; ++i)
    {
        SchedulerManager->WakeEventWaiters(notify[i]);
    }
}

void SynchronizationManager::WakeExitWord(virt_addr_space_t* space, uint64_t address)
{
    WakeKey(space, address, 1, UINT32_MAX);
    FutexKey key;

    if (key.Resolve(Resources->virtualMemoryManager, space, address, true) == 0)
    {
        WakeKey(key.identity, key.offset, 1, UINT32_MAX);
    }
}

uint64_t SynchronizationManager::Requeue(uint64_t address, uint64_t destination, int32_t wakeCount, int32_t moveCount, bool compare, uint32_t expected, bool shared)
{
    if (Resources == nullptr || Resources->processManager == nullptr || SchedulerManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if ((address & 3) != 0 || (destination & 3) != 0 || wakeCount < 0 || moveCount < 0)
    {
        return LINUX_EINVAL;
    }

    if (!range_within_u64(address, sizeof(uint32_t), CANONICAL_USER_BASE, CANONICAL_USER_END) || !range_within_u64(destination, sizeof(uint32_t), CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return LINUX_EFAULT;
    }

    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        return LINUX_ESRCH;
    }

    const uint64_t* notify[CAPACITY];
    size_t          woken = 0;
    size_t          moved = 0;
    uint64_t        result = 0;
    FutexKey sourceKey, destinationKey;
    result = sourceKey.Resolve(Resources->virtualMemoryManager, access.space, address, shared);

    if (result == 0)
    {
        result = destinationKey.Resolve(Resources->virtualMemoryManager, access.space, destination, shared);
    }

    if (result != 0)
    {
        end_user_memory(&access);

        return result;
    }
    spinlock_acquire_preempt(&Lock);
    uint32_t actual = 0;

    if (IsPriorityKeyLocked(sourceKey.identity, sourceKey.offset) || IsPriorityKeyLocked(destinationKey.identity, destinationKey.offset) || IsRequeueKeyLocked(sourceKey.identity, sourceKey.offset) || IsRequeueKeyLocked(destinationKey.identity, destinationKey.offset))
    {
        result = LINUX_EINVAL;
    }
    else if (compare && !safe_atomic_load_user32_locked(&access, address, &actual))
    {
        result = LINUX_EFAULT;
    }
    else if (compare && actual != expected)
    {
        result = (uint64_t) -11; // EAGAIN; neither queue has changed.
    }
    else
    {
        woken              = WakeKeyLocked(sourceKey.identity, sourceKey.offset, (uint32_t) wakeCount, UINT32_MAX, notify);
        size_t       index = Head;
        const size_t last  = Tail;

        while (index != NONE && moved < (uint32_t) moveCount)
        {
            waiter_t&    waiter = Waiters[index];
            const size_t next   = waiter.next;

            if (waiter.identity == sourceKey.identity && waiter.offset == sourceKey.offset)
            {
                Remove(index);
                VirtualMemoryManager::RetainBacking(destinationKey.object);
                VirtualMemoryManager::ReleaseBacking(waiter.object); // sourceKey keeps the old backing alive.
                waiter.object   = destinationKey.object;
                waiter.identity = destinationKey.identity;
                waiter.offset   = destinationKey.offset;
                waiter.next     = NONE;
                waiter.queued   = true;

                if (Tail != NONE)
                {
                    Waiters[Tail].next = index;
                }
                else
                {
                    Head = index;
                }

                Tail = index;
                ++moved;
            }

            if (index == last)
            {
                break;
            }

            index = next;
        }

        result = woken + moved;
    }

    spinlock_release_preempt(&Lock);
    end_user_memory(&access);
    NotifyWaiters(notify, woken);
    return result;
}

uint64_t SynchronizationManager::WakeOperation(uint64_t address, uint64_t destination, int32_t wakeCount, int32_t secondCount, uint32_t operation, bool shared)
{
    if (Resources == nullptr || Resources->processManager == nullptr || SchedulerManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if ((address & 3) != 0 || (destination & 3) != 0)
    {
        return LINUX_EINVAL;
    }

    if (!range_within_u64(address, sizeof(uint32_t), CANONICAL_USER_BASE, CANONICAL_USER_END) || !range_within_u64(destination, sizeof(uint32_t), CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return LINUX_EFAULT;
    }

    const uint32_t command  = (operation >> 28) & 7;
    const uint32_t compare  = (operation >> 24) & 15;
    const int32_t  operand  = (int32_t) (((operation >> 12) & 0xfffU) << 20) >> 20;
    const int32_t  expected = (int32_t) ((operation & 0xfffU) << 20) >> 20;

    if (command > 4)
    {
        return LINUX_ENOSYS;
    }

    const uint32_t argument = (operation & 0x80000000U) != 0 ? 1U << ((uint32_t) operand & 31U) : (uint32_t) operand;
    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        return LINUX_ESRCH;
    }

    const uint64_t* notify[CAPACITY];
    size_t          woken = 0;
    uint64_t        result = 0;
    FutexKey sourceKey, destinationKey;
    result = sourceKey.Resolve(Resources->virtualMemoryManager, access.space, address, shared);

    if (result == 0)
    {
        result = destinationKey.Resolve(Resources->virtualMemoryManager, access.space, destination, shared);
    }

    if (result != 0)
    {
        end_user_memory(&access);

        return result;
    }
    spinlock_acquire_preempt(&Lock);
    uint32_t actual = 0;

    if (IsPriorityKeyLocked(sourceKey.identity, sourceKey.offset) || IsPriorityKeyLocked(destinationKey.identity, destinationKey.offset) || IsRequeueKeyLocked(sourceKey.identity, sourceKey.offset) || IsRequeueKeyLocked(destinationKey.identity, destinationKey.offset))
    {
        result = LINUX_EINVAL;
    }
    else if (!safe_atomic_load_user32_locked(&access, destination, &actual))
    {
        result = LINUX_EFAULT;
    }
    else
    {
        for (;;)
        {
            const uint32_t previous = actual;
            uint32_t       next     = argument;

            switch (command)
            {
                case 1:
                    next = previous + argument;
                    break;
                case 2:
                    next = previous | argument;
                    break;
                case 3:
                    next = previous & ~argument;
                    break;
                case 4:
                    next = previous ^ argument;
                    break;
            }

            if (!safe_atomic_compare_exchange_user32_locked(&access, destination, &actual, next))
            {
                result = LINUX_EFAULT;
                break;
            }

            if (actual != previous)
            {
                continue;
            }

            // Linux reports an invalid comparison after the atomic update.
            if (compare > 5)
            {
                result = LINUX_ENOSYS;
                break;
            }

            const int32_t old     = (int32_t) previous;
            bool          matches = false;

            switch (compare)
            {
                case 0:
                    matches = old == expected;
                    break;
                case 1:
                    matches = old != expected;
                    break;
                case 2:
                    matches = old < expected;
                    break;
                case 3:
                    matches = old <= expected;
                    break;
                case 4:
                    matches = old > expected;
                    break;
                case 5:
                    matches = old >= expected;
                    break;
            }

            // WAKE_OP shares legacy WAKE's nonpositive-count behavior.
            const uint32_t firstLimit  = wakeCount > 0 ? (uint32_t) wakeCount : 1U;
            const uint32_t secondLimit = secondCount > 0 ? (uint32_t) secondCount : 1U;
            woken                      = WakeKeyLocked(sourceKey.identity, sourceKey.offset, firstLimit, UINT32_MAX, notify);

            if (matches)
            {
                woken += WakeKeyLocked(destinationKey.identity, destinationKey.offset, secondLimit, UINT32_MAX, notify + woken);
            }

            result = woken;
            break;
        }
    }

    spinlock_release_preempt(&Lock);
    end_user_memory(&access);
    NotifyWaiters(notify, woken);
    return result;
}

uint64_t SynchronizationManager::SetRobustList(uint64_t head)
{
    if (Resources == nullptr || Resources->taskManager == nullptr || Resources->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    task_t* task = Resources->taskManager->GetCurrentTask();

    if (task == nullptr)
    {
        return LINUX_ESRCH;
    }

    if (head != 0 && task->robustOwner == nullptr)
    {
        task->robustOwner = Resources->processManager->AcquireCurrentMemoryProcess(false);

        if (task->robustOwner == nullptr)
        {
            return LINUX_ESRCH;
        }
    }

    __atomic_store_n(&task->robustHead, head, __ATOMIC_RELEASE);
    task->retirementContext = this;
    task->retirementCleanup = head != 0 || task->clearChildTid != 0 ? CleanupRobust : nullptr;

    if (head == 0 && task->clearChildTid == 0 && task->robustOwner != nullptr)
    {
        Resources->processManager->ReleaseMemoryProcess(task->robustOwner);
        task->robustOwner = nullptr;
    }

    return 0;
}

void SynchronizationManager::ConfigureChildTid(task_t* task, process_t* owner, uint64_t address)
{
    task->clearChildTid = address;

    if (address != 0 && task->robustOwner == nullptr && owner != nullptr)
    {
        Resources->processManager->RetainMemoryProcess(owner);
        task->robustOwner = owner;
    }

    if (task->robustHead != 0 || address != 0)
    {
        task->retirementContext = this;
        task->retirementCleanup = CleanupRobust;
    }
    else if (task->robustOwner != nullptr)
    {
        Resources->processManager->ReleaseMemoryProcess(task->robustOwner);
        task->robustOwner       = nullptr;
        task->retirementCleanup = nullptr;
    }
}

uint64_t SynchronizationManager::GetRobustList(int32_t tid, uint64_t* head, uint64_t* length)
{
    if (Resources == nullptr || Resources->taskManager == nullptr || Resources->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    task_t* task = Resources->taskManager->GetCurrentTask();

    if (task == nullptr)
    {
        return LINUX_ESRCH;
    }

    if (tid < 0)
    {
        return LINUX_ESRCH;
    }

    if (tid == 0 || (uint64_t) tid == task->id)
    {
        *head   = __atomic_load_n(&task->robustHead, __ATOMIC_ACQUIRE);
        *length = 24;
        return 0;
    }

    if ((uint32_t) tid >= TaskManager::MAX_TASKS)
    {
        return LINUX_ESRCH;
    }

    process_t* process = Resources->processManager->AcquireCurrentMemoryProcess(false);

    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    task_t*  target = Resources->taskManager->FindTask(static_cast<uint32_t>(tid));
    uint64_t result = LINUX_ESRCH;

    if (Resources->taskManager->RetainForDispatch(target))
    {
        if (target->process != process)
        {
            // Cross-process inspection needs a ptrace/dumpability policy.
            result = LINUX_EOPNOTSUPP;
        }
        else
        {
            *head   = __atomic_load_n(&target->robustHead, __ATOMIC_ACQUIRE);
            *length = 24;
            result  = 0;
        }

        Resources->taskManager->ReleaseDispatchClaim(target);
    }

    Resources->processManager->ReleaseMemoryProcess(process);
    return result;
}

void SynchronizationManager::CleanupRobust(void* context, task_t* task)
{
    auto           manager  = static_cast<SynchronizationManager*>(context);
    process_t*     owner    = task->robustOwner;
    const uint64_t address  = __atomic_exchange_n(&task->robustHead, 0ULL, __ATOMIC_ACQ_REL);
    const uint64_t clearTid = __atomic_exchange_n(&task->clearChildTid, 0ULL, __ATOMIC_ACQ_REL);
    task->robustOwner       = nullptr;
    task->retirementCleanup = nullptr;

    if (owner == nullptr)
    {
        return;
    }

    cpu_preemption_disable();
    spinlock_acquire(&owner->addressSpace->operation_lock);
    user_memory_access_t access = {manager->Resources->processManager, owner, owner->addressSpace};
    const uint32_t zero = 0;

    if (clearTid != 0 && safe_write_user_locked(&access, clearTid, &zero, sizeof(zero)))
    {
        manager->WakeExitWord(owner->addressSpace, clearTid);
    }
    struct
    {
        uint64_t next;
        int64_t  offset;
        uint64_t pending;
    } head;

    if (address != 0 && safe_read_user_locked(&access, address, &head, sizeof(head)))
    {
        auto release = [&](uint64_t entry)
        {
            const bool priority = (entry & 1) != 0;
            entry &= ~1ULL;
            uint64_t word;

            if (head.offset >= 0)
            {
                if (entry > UINT64_MAX - (uint64_t) head.offset)
                {
                    return;
                }

                word = entry + (uint64_t) head.offset;
            }
            else
            {
                uint64_t magnitude = (uint64_t) (-(head.offset + 1)) + 1;

                if (entry < magnitude)
                {
                    return;
                }

                word = entry - magnitude;
            }

            uint32_t value;

            if (!safe_atomic_load_user32_locked(&access, word, &value))
            {
                return;
            }

            for (unsigned retry = 0; retry < 1024; ++retry)
            {
                if (value == 0 || (value & 0x3fffffffU) != task->id)
                {
                    return;
                }

                const uint32_t previous    = value;
                const uint32_t replacement = (value & 0x80000000U) | 0x40000000U;

                if (!safe_atomic_compare_exchange_user32_locked(&access, word, &value, replacement))
                {
                    return;
                }

                if (value == previous)
                {
                    if (!priority && (previous & 0x80000000U))
                    {
                        manager->WakeExitWord(owner->addressSpace, word);
                    }

                    return;
                }
            }
        };
        uint64_t entry = head.next;

        for (unsigned count = 0; count < 2048 && (entry & ~1ULL) != address && (entry & ~1ULL) != 0; ++count)
        {
            uint64_t next;

            if (!safe_read_user_locked(&access, entry & ~1ULL, &next, sizeof(next)))
            {
                break;
            }

            if (entry != head.pending)
            {
                release(entry);
            }

            entry = next;
        }

        if (head.pending != 0)
        {
            release(head.pending);
        }
    }

    spinlock_release(&owner->addressSpace->operation_lock);
    cpu_preemption_enable();
    manager->Resources->processManager->ReleaseMemoryProcess(owner);
}

size_t SynchronizationManager::BestPiWaiter(size_t state) const
{
    auto rank = [](uint32_t policy) -> unsigned
    {
        return policy == SCHEDULING_DEADLINE ? 2 : policy == SCHEDULING_FIFO || policy == SCHEDULING_RR ? 1 : 0;
    };
    auto better = [&](const pi_waiter_t& candidate, const pi_waiter_t& previous)
    {
        const auto& first = *candidate.task->scheduling;
        const auto& second = *previous.task->scheduling;
        const unsigned firstClass = rank(first.effectivePolicy), secondClass = rank(second.effectivePolicy);
        if (firstClass != secondClass)
        {
            return firstClass > secondClass;
        }
        if (firstClass == 2)
        {
            const auto donor = first.deadlineDonor != nullptr ? first.deadlineDonor : candidate.task;
            const auto other = second.deadlineDonor != nullptr ? second.deadlineDonor : previous.task;
            if (donor->scheduling->deadlineAbsolute != other->scheduling->deadlineAbsolute)
            {
                return donor->scheduling->deadlineAbsolute < other->scheduling->deadlineAbsolute;
            }
        }
        else if (firstClass == 1 && first.effectivePriority != second.effectivePriority)
        {
            return first.effectivePriority > second.effectivePriority;
        }
        else if (firstClass == 0 && candidate.task->effectiveNice != previous.task->effectiveNice)
        {
            return candidate.task->effectiveNice < previous.task->effectiveNice;
        }
        return candidate.order < previous.order;
    };
    size_t best = NONE;
    for (size_t i = 0; i < CAPACITY; ++i)
    {
        const auto& waiter = PiWaiters[i];
        if (waiter.used && waiter.state == state && !waiter.task->dying && (best == NONE || better(waiter, PiWaiters[best])))
        {
            best = i;
        }
    }
    return best;
}

void SynchronizationManager::RefreshPrioritiesLocked()
{
    task_t* tasks = Resources->taskManager->GetTasks();
    int32_t nice[CAPACITY];
    uint32_t policies[CAPACITY];
    uint32_t priorities[CAPACITY];
    task_t* donors[CAPACITY];
    bool retained[CAPACITY];
    auto rank = [](uint32_t policy) -> unsigned
    {
        return policy == SCHEDULING_DEADLINE ? 2 : policy == SCHEDULING_FIFO || policy == SCHEDULING_RR ? 1 : 0;
    };
    for (size_t i = 0; i < CAPACITY; ++i)
    {
        retained[i] = Resources->taskManager->RetainForDispatch(&tasks[i]);
        nice[i] = __atomic_load_n(&tasks[i].baseNice, __ATOMIC_ACQUIRE);
        const auto state = retained[i] ? tasks[i].scheduling : nullptr;
        policies[i] = state != nullptr ? state->policy : SCHEDULING_OTHER;
        priorities[i] = state != nullptr ? state->priority : 0;
        donors[i] = policies[i] == SCHEDULING_DEADLINE ? &tasks[i] : nullptr;
    }
    for (size_t pass = 0; pass < CAPACITY; ++pass)
    {
        bool changed = false;
        for (const auto& waiter : PiWaiters)
        {
            task_t* owner = waiter.used ? PiStates[waiter.state].owner : nullptr;
            if (owner == nullptr || !retained[owner - tasks] || !retained[waiter.task - tasks] || waiter.task->dying)
            {
                continue;
            }
            const size_t destination = owner - tasks;
            const size_t source = waiter.task - tasks;
            if (nice[destination] > nice[source])
            {
                nice[destination] = nice[source];
                changed = true;
            }
            bool better = rank(policies[source]) > rank(policies[destination]);
            if (rank(policies[source]) == rank(policies[destination]))
            {
                if (rank(policies[source]) == 2 && donors[source] != nullptr)
                {
                    better |= donors[destination] == nullptr || donors[source]->scheduling->deadlineAbsolute < donors[destination]->scheduling->deadlineAbsolute;
                }
                else if (rank(policies[source]) == 1)
                {
                    better |= priorities[source] > priorities[destination];
                }
            }
            if (better)
            {
                policies[destination] = policies[source];
                priorities[destination] = priorities[source];
                donors[destination] = donors[source];
                changed = true;
            }
        }
        if (!changed)
        {
            break;
        }
    }
    uint64_t reschedule = 0;
    for (size_t i = 0; i < CAPACITY; ++i)
    {
        if (!retained[i])
        {
            continue;
        }
        auto& state = *tasks[i].scheduling;
        const bool changed = tasks[i].effectiveNice != nice[i] || state.effectivePolicy != policies[i] || state.effectivePriority != priorities[i] || state.deadlineDonor != (donors[i] == &tasks[i] ? nullptr : donors[i]);
        SchedulerManager->ApplyPriorityDonation(&tasks[i], nice[i], policies[i], priorities[i], donors[i]);
        if (changed)
        {
            reschedule |= Scheduler::AvailableCpuMask();
        }
    }
    for (size_t i = 0; i < CAPACITY; ++i)
    {
        if (retained[i])
        {
            Resources->taskManager->ReleaseDispatchClaim(&tasks[i]);
        }
    }
    __atomic_fetch_or(&PriorityRescheduleMask, reschedule, __ATOMIC_RELEASE);
}

void SynchronizationManager::RefreshPriorities()
{
    spinlock_acquire_preempt(&Lock);
    RefreshPrioritiesLocked();
    spinlock_release_preempt(&Lock);
    SchedulerManager->RequestReschedule(__atomic_exchange_n(&PriorityRescheduleMask, 0ULL, __ATOMIC_ACQ_REL));
}

uint64_t SynchronizationManager::PriorityFutex(uint64_t address, uint32_t command, uint64_t deadline, bool shared)
{
    if (!arch_task_context_supported())
    {
        return LINUX_EOPNOTSUPP;
    }
    if (Resources == nullptr || Resources->processManager == nullptr || Resources->taskManager == nullptr || SchedulerManager == nullptr)
    {
        return LINUX_ENOSYS;
    }
    if ((address & 3) != 0 || (command != 6 && command != 7 && command != 8 && command != 13))
    {
        return LINUX_EINVAL;
    }
    auto       processes = Resources->processManager;
    auto       tasks     = Resources->taskManager;
    process_t* process   = processes->AcquireCurrentMemoryProcess(false);
    task_t*    task      = tasks->GetCurrentTask();
    if (process == nullptr || task == nullptr || task->id == 0 || task->id > 0x3fffffffU)
    {
        if (process != nullptr)
        {
            processes->ReleaseMemoryProcess(process);
        }
        return LINUX_ESRCH;
    }
    const size_t slot   = task - tasks->GetTasks();
    pi_waiter_t& waiter = PiWaiters[slot];
    uint64_t     result = 0;
    for (;;)
    {
        user_memory_access_t access;
        if (!begin_user_memory(&access))
        {
            result = LINUX_ESRCH;
            break;
        }
        FutexKey key;
        result = key.Resolve(Resources->virtualMemoryManager, access.space, address, shared);
        spinlock_acquire_preempt(&Lock);
        size_t stateIndex = NONE;
        size_t freeIndex  = NONE;
        for (size_t i = 0; i < CAPACITY; ++i)
        {
            if (PiStates[i].used && PiStates[i].identity == key.identity && PiStates[i].offset == key.offset)
            {
                stateIndex = i;
            }
            if (!PiStates[i].used && freeIndex == NONE)
            {
                freeIndex = i;
            }
            if (Waiters[i].used && Waiters[i].identity == key.identity && Waiters[i].offset == key.offset)
            {
                result = LINUX_EINVAL;
            }
        }
        if (waiter.used && (stateIndex == NONE || waiter.state != stateIndex))
        {
            result = LINUX_EINVAL;
        }
        uint32_t word = 0;
        if (result == 0 && !safe_atomic_load_user32_locked(&access, address, &word))
        {
            result = LINUX_EFAULT;
        }
        const uint32_t         tid            = word & 0x3fffffffU;
        process_t*             releaseProcess = nullptr;
        virt_backing_object_t* releaseObject  = nullptr;
        bool                   acquired       = false;
        bool                   retry          = false;
        bool                   wait           = false;
        const uint64_t*        notify[CAPACITY];
        size_t                 notifyCount = 0;
        // An uncontended userspace CAS may have released and reacquired the
        // word without entering the kernel. Reconcile only when no queued
        // waiter depends on the recorded owner; contended ownership is strict.
        if (result == 0 && stateIndex != NONE && PiStates[stateIndex].owner != nullptr && tid != PiStates[stateIndex].owner->id && !HasPiWaiters(stateIndex))
        {
            pi_state_t& state = PiStates[stateIndex];
            releaseProcess    = state.memoryOwner;
            state.memoryOwner = nullptr;
            state.owner       = nullptr;
            state.dead        = (word & 0x40000000U) != 0;
            if (tid != 0)
            {
                task_t* owner = tasks->FindTask(tid);
                if (!tasks->RetainForDispatch(owner))
                {
                    result = LINUX_ESRCH;
                }
                else
                {
                    if (owner->process == nullptr || (!shared && owner->process->addressSpace != process->addressSpace))
                    {
                        result = LINUX_EINVAL;
                    }
                    else
                    {
                        state.owner = owner;
                    }
                    tasks->ReleaseDispatchClaim(owner);
                }
            }
        }
        if (result == 0 && command == 7)
        {
            if (tid != task->id || (stateIndex != NONE && PiStates[stateIndex].owner != task))
            {
                result = static_cast<uint64_t>(-1);
            }
            else
            {
                const uint32_t expected = word;
                // Reserve a contended handoff with a nonzero WAITERS word.
                // An uncontended userspace CAS must not steal the lock while
                // the selected kernel waiter is resuming.
                const uint32_t replacement = stateIndex != NONE && HasPiWaiters(stateIndex) ? 0x80000000U : 0;
                if (!safe_atomic_compare_exchange_user32_locked(&access, address, &word, replacement))
                {
                    result = LINUX_EFAULT;
                }
                else if (word != expected)
                {
                    result = LINUX_EINVAL;
                }
                else if (stateIndex != NONE)
                {
                    pi_state_t& state = PiStates[stateIndex];
                    if (state.memoryOwner != nullptr)
                    {
                        releaseProcess = state.memoryOwner;
                    }
                    state.memoryOwner = nullptr;
                    state.owner       = nullptr;
                    for (auto& pending : PiWaiters)
                    {
                        if (pending.used && pending.state == stateIndex)
                        {
                            __atomic_add_fetch(&pending.sequence, 1ULL, __ATOMIC_RELEASE);
                            notify[notifyCount++] = &pending.sequence;
                        }
                    }
                    if (!HasPiWaiters(stateIndex))
                    {
                        state.used    = false;
                        releaseObject = state.object;
                        state.object  = nullptr;
                    }
                }
            }
        }
        else if (result == 0)
        {
            if (stateIndex == NONE && freeIndex == NONE)
            {
                result = static_cast<uint64_t>(-11);
            }
            else
            {
                if (stateIndex == NONE)
                {
                    stateIndex        = freeIndex;
                    pi_state_t& state = PiStates[stateIndex];
                    state             = {};
                    state.used        = true;
                    state.identity    = key.identity;
                    state.offset      = key.offset;
                    state.object      = key.object;
                    VirtualMemoryManager::RetainBacking(state.object);
                    state.dead = (word & 0x40000000U) != 0;
                    if (tid != 0)
                    {
                        task_t* owner = tasks->FindTask(tid);
                        if (!tasks->RetainForDispatch(owner))
                        {
                            result = LINUX_ESRCH;
                        }
                        else
                        {
                            if (owner->process == nullptr || (!shared && owner->process->addressSpace != process->addressSpace))
                            {
                                result = LINUX_EINVAL;
                            }
                            else
                            {
                                state.owner = owner;
                            }
                            tasks->ReleaseDispatchClaim(owner);
                        }
                    }
                }
                pi_state_t& state = PiStates[stateIndex];
                if (result == 0 && state.owner == task)
                {
                    result = static_cast<uint64_t>(-35); // EDEADLK
                }
                if (result == 0 && ((state.owner != nullptr && tid != state.owner->id) || (state.owner == nullptr && tid != 0 && !state.dead)))
                {
                    result = LINUX_EINVAL;
                }
                const size_t best = BestPiWaiter(stateIndex);
                if (result == 0 && state.owner == nullptr && (best == NONE || best == slot))
                {
                    const uint32_t expected    = word;
                    const uint32_t replacement = static_cast<uint32_t>(task->id) | ((state.dead || (word & 0x40000000U)) ? 0x40000000U : 0) | (best != NONE ? 0x80000000U : 0);
                    if (!safe_atomic_compare_exchange_user32_locked(&access, address, &word, replacement))
                    {
                        result = LINUX_EFAULT;
                    }
                    else if (word != expected)
                    {
                        retry = true;
                    }
                    else
                    {
                        state.owner       = task;
                        state.dead        = false;
                        state.memoryOwner = process;
                        processes->RetainMemoryProcess(process);
                        state.address = address;
                        waiter.used   = false;
                        acquired      = true;
                    }
                }
                else if (result == 0 && command == 8)
                {
                    result = static_cast<uint64_t>(-11);
                }
                else if (result == 0)
                {
                    task_t* owner = state.owner;
                    for (size_t depth = 0; owner != nullptr && depth < CAPACITY; ++depth)
                    {
                        if (task->scheduling->effectivePolicy == SCHEDULING_DEADLINE)
                        {
                            task_t* donor = task->scheduling->deadlineDonor != nullptr ? task->scheduling->deadlineDonor : task;
                            const int16_t cpu = donor->scheduling->reservationCpu;
                            if (cpu < 0 || !(owner->affinityMask & (1ULL << cpu)))
                            {
                                result = LINUX_EOPNOTSUPP;
                                break;
                            }
                        }
                        if (owner == task)
                        {
                            result = static_cast<uint64_t>(-35);
                            break;
                        }
                        pi_waiter_t& edge = PiWaiters[owner - tasks->GetTasks()];
                        owner             = edge.used ? PiStates[edge.state].owner : nullptr;
                    }
                    if (result == 0)
                    {
                        const uint32_t expected = word;
                        if (!safe_atomic_compare_exchange_user32_locked(&access, address, &word, word | 0x80000000U))
                        {
                            result = LINUX_EFAULT;
                        }
                        else if (word != expected)
                        {
                            retry = true;
                        }
                        else
                        {
                            if (!waiter.used)
                            {
                                waiter.used  = true;
                                waiter.state = stateIndex;
                                waiter.task  = task;
                                waiter.order = ++PiOrder;
                            }
                            wait = true;
                        }
                    }
                }
                if (result != 0 && state.owner == nullptr && !HasPiWaiters(stateIndex))
                {
                    state.used    = false;
                    releaseObject = state.object;
                    state.object  = nullptr;
                }
            }
        }
        if (result != 0 && stateIndex != NONE && PiStates[stateIndex].used && PiStates[stateIndex].owner == nullptr && !HasPiWaiters(stateIndex))
        {
            PiStates[stateIndex].used   = false;
            releaseObject               = PiStates[stateIndex].object;
            PiStates[stateIndex].object = nullptr;
        }
        RefreshPrioritiesLocked();
        const uint64_t sequence = __atomic_load_n(&waiter.sequence, __ATOMIC_ACQUIRE);
        spinlock_release_preempt(&Lock);
        end_user_memory(&access);
        if (releaseProcess != nullptr)
        {
            processes->ReleaseMemoryProcess(releaseProcess);
        }
        VirtualMemoryManager::ReleaseBacking(releaseObject);
        NotifyWaiters(notify, notifyCount);
        if (result != 0 || command == 7 || acquired)
        {
            break;
        }
        if (retry)
        {
            continue;
        }
        uint64_t now = 0;
        if (deadline != UINT64_MAX && (!arch_time_read_monotonic(&now) || now >= deadline))
        {
            result = static_cast<uint64_t>(-110);
            break;
        }
        if (wait)
        {
            result = SchedulerManager->WaitForEvent(&waiter.sequence, sequence, deadline);
            if (result != 0)
            {
                break;
            }
        }
    }
    spinlock_acquire_preempt(&Lock);
    size_t     stateIndex = waiter.state;
    const bool cancelled  = waiter.used;
    waiter.used           = false;
    RefreshPrioritiesLocked();
    const uint64_t*        notify[CAPACITY];
    size_t                 notifyCount   = 0;
    virt_backing_object_t* releaseObject = nullptr;
    if (cancelled)
    {
        for (auto& pending : PiWaiters)
        {
            if (pending.used && pending.state == stateIndex)
            {
                __atomic_add_fetch(&pending.sequence, 1ULL, __ATOMIC_RELEASE);
                notify[notifyCount++] = &pending.sequence;
            }
        }
        pi_state_t& state = PiStates[stateIndex];
        if (state.owner == nullptr && !HasPiWaiters(stateIndex))
        {
            state.used    = false;
            releaseObject = state.object;
            state.object  = nullptr;
        }
    }
    spinlock_release_preempt(&Lock);
    VirtualMemoryManager::ReleaseBacking(releaseObject);
    NotifyWaiters(notify, notifyCount);
    processes->ReleaseMemoryProcess(process);
    return result;
}

void SynchronizationManager::CleanupPriority(void* context, task_t* task)
{
    auto manager = static_cast<SynchronizationManager*>(context);
    if (task->dying)
    {
        manager->SchedulerManager->RetirePolicy(task);
    }
    for (size_t i = 0; i < CAPACITY; ++i)
    {
        spinlock_acquire_preempt(&manager->Lock);
        pi_state_t& state = manager->PiStates[i];
        if (!state.used || state.owner != task)
        {
            spinlock_release_preempt(&manager->Lock);
            continue;
        }
        process_t*     process = state.memoryOwner;
        const uint64_t address = state.address;
        state.memoryOwner      = nullptr;
        state.owner            = nullptr;
        state.dead             = true;
        const uint64_t* notify[CAPACITY];
        size_t          count = 0;
        for (auto& waiter : manager->PiWaiters)
        {
            if (waiter.used && waiter.state == i)
            {
                __atomic_add_fetch(&waiter.sequence, 1ULL, __ATOMIC_RELEASE);
                notify[count++] = &waiter.sequence;
            }
        }
        virt_backing_object_t* object = nullptr;
        if (!manager->HasPiWaiters(i))
        {
            state.used   = false;
            object       = state.object;
            state.object = nullptr;
        }
        manager->RefreshPrioritiesLocked();
        spinlock_release_preempt(&manager->Lock);
        if (process != nullptr)
        {
            cpu_preemption_disable();
            spinlock_acquire(&process->addressSpace->operation_lock);
            user_memory_access_t access = {manager->Resources->processManager, process, process->addressSpace};
            uint32_t             word;
            if (safe_atomic_load_user32_locked(&access, address, &word))
            {
                for (size_t attempt = 0; attempt < 128 && (word & 0x3fffffffU) == task->id; ++attempt)
                {
                    const uint32_t previous = word;
                    if (!safe_atomic_compare_exchange_user32_locked(&access, address, &word, (word & 0x80000000U) | 0x40000000U) || word == previous)
                    {
                        break;
                    }
                }
            }
            spinlock_release(&process->addressSpace->operation_lock);
            cpu_preemption_enable();
            manager->Resources->processManager->ReleaseMemoryProcess(process);
        }
        VirtualMemoryManager::ReleaseBacking(object);
        manager->NotifyWaiters(notify, count);
    }
}

bool SynchronizationManager::HasPiWaiters(size_t state) const
{
    for (const auto& waiter : PiWaiters)
    {
        if (waiter.used && waiter.state == state)
        {
            return true;
        }
    }
    return false;
}

bool SynchronizationManager::IsPriorityKeyLocked(void* identity, uint64_t offset) const
{
    for (const auto& state : PiStates)
    {
        if (state.used && state.identity == identity && state.offset == offset)
        {
            return true;
        }
    }
    return false;
}

bool SynchronizationManager::IsRequeueKeyLocked(void* identity, uint64_t offset) const
{
    for (const auto& waiter : Waiters)
    {
        if (waiter.used && waiter.requeuePriority && waiter.identity == identity && waiter.offset == offset)
        {
            return true;
        }
    }
    return false;
}

uint64_t SynchronizationManager::RequeuePriority(uint64_t address, uint64_t destination, int32_t wakeCount, int32_t moveCount, uint32_t expected, bool shared)
{
    if (!arch_task_context_supported())
    {
        return LINUX_EOPNOTSUPP;
    }
    if ((address & 3) != 0 || (destination & 3) != 0 || wakeCount != 1 || moveCount < 0)
    {
        return LINUX_EINVAL;
    }
    user_memory_access_t access;
    if (!begin_user_memory(&access))
    {
        return LINUX_ESRCH;
    }
    FutexKey sourceKey, destinationKey;
    uint64_t result = sourceKey.Resolve(Resources->virtualMemoryManager, access.space, address, shared);
    if (result == 0)
    {
        result = destinationKey.Resolve(Resources->virtualMemoryManager, access.space, destination, shared);
    }
    const uint64_t* notify[CAPACITY];
    size_t count = 0;
    spinlock_acquire_preempt(&Lock);
    uint32_t source = 0, word = 0;
    size_t stateIndex = NONE, freeIndex = NONE;
    if (result == 0 && (sourceKey.identity == destinationKey.identity && sourceKey.offset == destinationKey.offset))
    {
        result = LINUX_EINVAL;
    }
    if (result == 0 && (!safe_atomic_load_user32_locked(&access, address, &source) || !safe_atomic_load_user32_locked(&access, destination, &word)))
    {
        result = LINUX_EFAULT;
    }
    if (result == 0 && source != expected)
    {
        result = LINUX_EAGAIN;
    }
    for (size_t i = 0; result == 0 && i < CAPACITY; ++i)
    {
        const auto& state = PiStates[i];
        if (!state.used && freeIndex == NONE)
        {
            freeIndex = i;
        }
        if (state.used && state.identity == destinationKey.identity && state.offset == destinationKey.offset)
        {
            stateIndex = i;
        }
        const auto& waiter = Waiters[i];
        if (waiter.used && waiter.identity == destinationKey.identity && waiter.offset == destinationKey.offset)
        {
            result = LINUX_EINVAL;
        }
        if (waiter.queued && waiter.identity == sourceKey.identity && waiter.offset == sourceKey.offset && (!waiter.requeuePriority || waiter.destinationIdentity != destinationKey.identity || waiter.destinationOffset != destinationKey.offset))
        {
            result = LINUX_EINVAL;
        }
    }
    if (result == 0 && IsPriorityKeyLocked(sourceKey.identity, sourceKey.offset))
    {
        result = LINUX_EINVAL;
    }
    task_t* owner = nullptr;
    bool ownerClaim = false;
    const uint32_t tid = word & 0x3fffffffU;
    if (result == 0 && tid != 0)
    {
        owner = Resources->taskManager->FindTask(tid);
        ownerClaim = Resources->taskManager->RetainForDispatch(owner);
        if (!ownerClaim)
        {
            result = LINUX_ESRCH;
        }
        else if (owner->process == nullptr || (!shared && owner->process->addressSpace != access.space))
        {
            result = LINUX_EINVAL;
        }
    }
    if (result == 0 && stateIndex != NONE && PiStates[stateIndex].owner != owner)
    {
        result = LINUX_EINVAL;
    }
    // Validate every edge before changing either queue.
    for (size_t i = Head; result == 0 && i != NONE; i = Waiters[i].next)
    {
        auto& waiter = Waiters[i];
        if (waiter.identity != sourceKey.identity || waiter.offset != sourceKey.offset)
        {
            continue;
        }
        task_t* ancestor = owner;
        for (size_t depth = 0; ancestor != nullptr && depth < CAPACITY; ++depth)
        {
            if (ancestor == waiter.task)
            {
                result = static_cast<uint64_t>(-35);
                break;
            }
            if (waiter.task->scheduling->effectivePolicy == SCHEDULING_DEADLINE)
            {
                task_t* donor = waiter.task->scheduling->deadlineDonor != nullptr ? waiter.task->scheduling->deadlineDonor : waiter.task;
                const int cpu = donor->scheduling->reservationCpu;
                if (cpu < 0 || !(ancestor->affinityMask & (1ULL << cpu)))
                {
                    result = LINUX_EOPNOTSUPP;
                    break;
                }
            }
            const auto& edge = PiWaiters[ancestor - Resources->taskManager->GetTasks()];
            ancestor = edge.used ? PiStates[edge.state].owner : nullptr;
        }
        ++count;
        if (count >= static_cast<uint64_t>(moveCount) + 1)
        {
            break;
        }
    }
    if (result == 0 && count != 0)
    {
        if (stateIndex == NONE && freeIndex == NONE)
        {
            result = LINUX_EAGAIN;
        }
        else
        {
            const uint32_t previous = word;
            if (!safe_atomic_compare_exchange_user32_locked(&access, destination, &word, word | 0x80000000U))
            {
                result = LINUX_EFAULT;
            }
            else if (word != previous)
            {
                result = LINUX_EAGAIN;
            }
        }
    }
    size_t moved = 0;
    if (result == 0 && count != 0)
    {
        if (stateIndex == NONE)
        {
            stateIndex = freeIndex;
            auto& state = PiStates[stateIndex];
            state = {};
            state.used = true;
            state.dead = (word & 0x40000000U) != 0;
            state.identity = destinationKey.identity;
            state.offset = destinationKey.offset;
            state.object = destinationKey.object;
            state.owner = owner;
            VirtualMemoryManager::RetainBacking(state.object);
        }
        for (size_t index = Head; index != NONE && moved < count;)
        {
            auto& waiter = Waiters[index];
            const size_t next = waiter.next;
            if (waiter.identity == sourceKey.identity && waiter.offset == sourceKey.offset)
            {
                Remove(index);
                auto& edge = PiWaiters[index];
                edge.used = true;
                edge.task = waiter.task;
                edge.state = stateIndex;
                edge.order = ++PiOrder;
                waiter.converted = waiter.woken = true;
                __atomic_add_fetch(&waiter.sequence, 1ULL, __ATOMIC_RELEASE);
                notify[moved++] = &waiter.sequence;
            }
            index = next;
        }
        RefreshPrioritiesLocked();
    }
    if (ownerClaim)
    {
        Resources->taskManager->ReleaseDispatchClaim(owner);
    }
    spinlock_release_preempt(&Lock);
    end_user_memory(&access);
    NotifyWaiters(notify, moved);
    return result != 0 ? result : moved;
}

bool SynchronizationManager::LockSchedulingChange(task_t* task, uint64_t affinity)
{
    spinlock_acquire_preempt(&Lock);
    bool participant = PiWaiters[task - Resources->taskManager->GetTasks()].used;
    for (const auto& state : PiStates)
    {
        participant |= state.used && state.owner == task;
    }
    const auto& policy = *task->scheduling;
    bool blocked = affinity == 0 && participant;
    if (affinity != 0 && policy.effectivePolicy == SCHEDULING_DEADLINE)
    {
        task_t* donor = policy.deadlineDonor != nullptr ? policy.deadlineDonor : task;
        const int cpu = donor->scheduling->reservationCpu;
        blocked |= cpu < 0 || !(affinity & (1ULL << cpu));
        // A nonparticipating reservation can be re-admitted on another CPU.
        if (!participant && donor == task)
        {
            blocked = false;
        }
    }
    if (blocked)
    {
        spinlock_release_preempt(&Lock);
        return false;
    }
    return true;
}

void SynchronizationManager::UnlockSchedulingChange()
{
    spinlock_release_preempt(&Lock);
}
