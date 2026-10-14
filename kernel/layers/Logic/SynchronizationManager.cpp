#include "layers/Logic/SynchronizationManager.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include <klib/klib.h>

SynchronizationManager::SynchronizationManager(ResourceLayerCaps* resources, Scheduler* scheduler)
    : Resources(resources), SchedulerManager(scheduler), Lock(0), Waiters{}, Head(NONE), Tail(NONE)
{
    static_assert(CAPACITY == TaskManager::MAX_TASKS, "one futex waiter per task");
}

void SynchronizationManager::Remove(size_t index)
{
    size_t* link = &Head;
    size_t previous = NONE;
    while (*link != NONE && *link != index)
    {
        previous = *link;
        link = &Waiters[*link].next;
    }
    if (*link == NONE) return;
    *link = Waiters[index].next;
    if (Tail == index) Tail = previous;
    Waiters[index].queued = false;
}

uint64_t SynchronizationManager::Wait(uint64_t address, uint32_t expected, uint32_t bits, uint64_t deadline)
{
    if (Resources == nullptr || Resources->processManager == nullptr || Resources->taskManager == nullptr || SchedulerManager == nullptr) return LINUX_ENOSYS;
    if ((address & 3) != 0 || bits == 0) return LINUX_EINVAL;
    ProcessManager* processes = Resources->processManager;
    process_t* process = processes->AcquireCurrentMemoryProcess();
    if (process == nullptr) return LINUX_ESRCH;
    task_t* task = Resources->taskManager->GetCurrentTask();
    uint64_t result = [&]() -> uint64_t
    {
        if (task == nullptr || task->id >= CAPACITY) return LINUX_ESRCH;
        user_memory_access_t access;
        if (!begin_user_memory(&access)) return LINUX_ESRCH;
        const size_t index = (size_t) task->id;
        waiter_t& waiter = Waiters[index];
        spinlock_acquire_preempt(&Lock);
        uint32_t actual;
        uint64_t status = 0;
        if (waiter.used) status = LINUX_EINVAL;
        else if (!safe_atomic_load_user32_locked(&access, address, &actual)) status = LINUX_EFAULT;
        else if (actual != expected) status = (uint64_t) -11; // EAGAIN
        if (status == 0)
        {
            waiter.space = access.space;
            waiter.address = address;
            waiter.bits = bits;
            waiter.used = waiter.queued = true;
            waiter.woken = false;
            waiter.next = NONE;
            if (Tail != NONE) Waiters[Tail].next = index;
            else Head = index;
            Tail = index;
        }
        const uint64_t sequence = __atomic_load_n(&waiter.sequence, __ATOMIC_ACQUIRE);
        spinlock_release_preempt(&Lock);
        end_user_memory(&access);
        if (status != 0) return status;

        for (;;)
        {
            uint64_t now = 0;
            const bool clock = deadline == UINT64_MAX || arch_time_read_monotonic(&now);
            spinlock_acquire_preempt(&Lock);
            if (waiter.woken) status = 0;
            else if (!clock) status = LINUX_ENOSYS;
            else if (deadline != UINT64_MAX && now >= deadline) status = (uint64_t) -110; // ETIMEDOUT
            else
            {
                spinlock_release_preempt(&Lock);
                status = SchedulerManager->WaitForEvent(&waiter.sequence, sequence, deadline);
                if (status == 0) continue;
                spinlock_acquire_preempt(&Lock);
                // A concurrent wake already consumed this waiter; report that success.
                if (waiter.woken) status = 0;
            }
            // Slot and process lifetime remain protected until after dequeue.
            if (waiter.queued) Remove(index);
            waiter.used = false;
            waiter.space = nullptr;
            spinlock_release_preempt(&Lock);
            return status;
        }
    }();
    processes->ReleaseMemoryProcess(process);
    return result;
}

uint64_t SynchronizationManager::Wake(uint64_t address, int32_t count, uint32_t bits)
{
    if (Resources == nullptr || Resources->processManager == nullptr || SchedulerManager == nullptr) return LINUX_ENOSYS;
    if ((address & 3) != 0 || bits == 0) return LINUX_EINVAL;
    if (!range_within_u64(address, sizeof(uint32_t), CANONICAL_USER_BASE, CANONICAL_USER_END)) return LINUX_EFAULT;
    ProcessManager* processes = Resources->processManager;
    process_t* process = processes->AcquireCurrentMemoryProcess();
    if (process == nullptr) return LINUX_ESRCH;
    const uint64_t result = WakeKey(process->addressSpace, address, count, bits);
    processes->ReleaseMemoryProcess(process);
    return result;
}

uint64_t SynchronizationManager::WakeKey(virt_addr_space* space, uint64_t address, int32_t count, uint32_t bits)
{
    const uint64_t* notify[CAPACITY];
    size_t woken = 0;
    // Legacy Linux FUTEX_WAKE/WAKE_BITSET wakes the first matching waiter even
    // for a nonpositive nr_wake (the limit is checked after removing it).
    const uint32_t limit = count > 0 ? (uint32_t) count : 1U;
    spinlock_acquire_preempt(&Lock);
    size_t index = Head;
    while (index != NONE && woken < limit)
    {
        waiter_t& waiter = Waiters[index];
        const size_t next = waiter.next;
        if (waiter.space == space && waiter.address == address && (waiter.bits & bits) != 0)
        {
            Remove(index);
            waiter.woken = true;
            __atomic_add_fetch(&waiter.sequence, 1ULL, __ATOMIC_RELEASE);
            notify[woken++] = &waiter.sequence;
        }
        index = next;
    }
    spinlock_release_preempt(&Lock);
    for (size_t i = 0; i < woken; ++i) SchedulerManager->WakeEventWaiters(notify[i]);
    return woken;
}

uint64_t SynchronizationManager::SetRobustList(uint64_t head)
{
    if (Resources == nullptr || Resources->taskManager == nullptr || Resources->processManager == nullptr) return LINUX_ENOSYS;
    task_t* task = Resources->taskManager->GetCurrentTask();
    if (task == nullptr) return LINUX_ESRCH;
    if (head != 0 && task->robustOwner == nullptr)
    {
        task->robustOwner = Resources->processManager->AcquireCurrentMemoryProcess();
        if (task->robustOwner == nullptr) return LINUX_ESRCH;
    }
    task->robustHead = head;
    task->retirementContext = this;
    task->retirementCleanup = head != 0 ? CleanupRobust : nullptr;
    if (head == 0 && task->robustOwner != nullptr)
    {
        Resources->processManager->ReleaseMemoryProcess(task->robustOwner);
        task->robustOwner = nullptr;
    }
    return 0;
}

uint64_t SynchronizationManager::GetRobustList(int32_t tid, uint64_t* head, uint64_t* length)
{
    if (Resources == nullptr || Resources->taskManager == nullptr) return LINUX_ENOSYS;
    task_t* task = Resources->taskManager->GetCurrentTask();
    if (task == nullptr) return LINUX_ESRCH;
    if (tid < 0) return LINUX_ESRCH;
    if (tid != 0 && (uint64_t) tid != task->id) return LINUX_EOPNOTSUPP;
    *head = task->robustHead;
    *length = 24;
    return 0;
}

void SynchronizationManager::CleanupRobust(void* context, task_t* task)
{
    auto manager = static_cast<SynchronizationManager*>(context);
    process_t* owner = task->robustOwner;
    const uint64_t address = task->robustHead;
    task->robustHead = 0;
    task->robustOwner = nullptr;
    task->retirementCleanup = nullptr;
    if (owner == nullptr) return;
    cpu_preemption_disable();
    spinlock_acquire(&owner->addressSpace->operation_lock);
    user_memory_access_t access = {manager->Resources->processManager, owner, owner->addressSpace};
    struct { uint64_t next; int64_t offset; uint64_t pending; } head;
    if (address != 0 && safe_read_user_locked(&access, address, &head, sizeof(head)))
    {
        auto release = [&](uint64_t entry)
        {
            if ((entry & 1) != 0) return; // PI futexes remain unsupported.
            uint64_t word;
            if (head.offset >= 0)
            {
                if (entry > UINT64_MAX - (uint64_t) head.offset) return;
                word = entry + (uint64_t) head.offset;
            }
            else
            {
                uint64_t magnitude = (uint64_t) (-(head.offset + 1)) + 1;
                if (entry < magnitude) return;
                word = entry - magnitude;
            }
            uint32_t value;
            if (!safe_atomic_load_user32_locked(&access, word, &value)) return;
            for (unsigned retry = 0; retry < 1024; ++retry)
            {
                if (value == 0 || (value & 0x3fffffffU) != task->id) return;
                const uint32_t previous = value;
                const uint32_t replacement = (value & 0x80000000U) | 0x40000000U;
                if (!safe_atomic_compare_exchange_user32_locked(&access, word, &value, replacement)) return;
                if (value == previous)
                {
                    if (previous & 0x80000000U) manager->WakeKey(owner->addressSpace, word, 1, UINT32_MAX);
                    return;
                }
            }
        };
        uint64_t entry = head.next;
        for (unsigned count = 0; count < 2048 && (entry & ~1ULL) != address && (entry & ~1ULL) != 0; ++count)
        {
            uint64_t next;
            if (!safe_read_user_locked(&access, entry & ~1ULL, &next, sizeof(next))) break;
            if (entry != head.pending) release(entry);
            entry = next;
        }
        if (head.pending != 0) release(head.pending);
    }
    spinlock_release(&owner->addressSpace->operation_lock);
    cpu_preemption_enable();
    manager->Resources->processManager->ReleaseMemoryProcess(owner);
}
