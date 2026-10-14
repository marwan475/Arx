#include "layers/Logic/Scheduler.hpp"

#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <platform.h>
}

Scheduler::Scheduler(ResourceLayerCaps* resourceLayerCaps)
{
    ResourceLayerImportCaps = resourceLayerCaps;
    QueueLock               = 0;
    IdleAddressSpace        = platform.cpus[platform.bsp_id].address_space;
    memset(SleepQueue, 0, sizeof(SleepQueue));
    static_assert(SLEEP_QUEUE_CAPACITY == TaskManager::MAX_TASKS, "one sleep entry per task");

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        ReadyQueues[i].head  = 0;
        ReadyQueues[i].tail  = 0;
        ReadyQueues[i].count = 0;
        SchedulingActive[i]  = false;
        SleepHeads[i]        = NO_SLEEPER;
        IdleTasks[i]         = nullptr;
    }

    InitializeBspProcessAndTask();
    // Allocate idle contexts before timer dispatch can enter with IF cleared.
    // Switching away from exit never needs allocation or a TLB wait.
    for (size_t i = 0; i < platform.cpu_count; ++i)
    {
        IdleTasks[i] = ResourceLayerImportCaps->taskManager->CreateKernelTask(IdleEntry, this);
        if (IdleTasks[i] == nullptr)
            panic();
    }
}

void Scheduler::LockQueues()
{
    cpu_preemption_disable();
    spinlock_acquire(&QueueLock);
}

void Scheduler::UnlockQueues()
{
    spinlock_release(&QueueLock);
    cpu_preemption_enable();
}

void Scheduler::IdleEntry(void* argument)
{
    Scheduler*    scheduler = static_cast<Scheduler*>(argument);
    const uint8_t cpuId     = arch_cpu_id();
    for (;;)
    {
        scheduler->ReapExitedProcesses();
        scheduler->WakeSleepingTasks(cpuId);
        if (!scheduler->RunNextReadyProcess(cpuId))
        {
            // Periodic timer interrupts recheck deadlines even with no ready task.
#if defined(__x86_64__)
            __asm__ volatile("sti; hlt" : : : "memory");
#else
            arch_enable_interrupts();
            arch_halt();
#endif
        }
    }
}

void Scheduler::WakeSleepingTasks(uint8_t cpuId)
{
    if (cpuId != arch_cpu_id() || cpuId >= BOOT_SMP_MAX_CPUS)
        return;
    uint64_t now            = 0;
    bool     clockAvailable = arch_time_read_monotonic(&now);
    LockQueues();
    size_t* link = &SleepHeads[cpuId];
    while (*link != NO_SLEEPER)
    {
        sleep_entry_t& entry  = SleepQueue[*link];
        const bool     exited = __atomic_load_n(&entry.process->exited, __ATOMIC_ACQUIRE);
        if (!clockAvailable || exited || entry.deadline <= now)
        {
            *link         = entry.next;
            entry.next    = NO_SLEEPER;
            entry.waiting = false;
            entry.result  = !clockAvailable ? static_cast<uint64_t>(-38) : exited ? static_cast<uint64_t>(-3) : 0;
            __atomic_store_n(&entry.task->sleepBlocked, false, __ATOMIC_RELEASE);
        }
        else
            link = &entry.next;
    }
    UnlockQueues();
}

void Scheduler::ReapExitedProcesses()
{
    ProcessManager* manager = ResourceLayerImportCaps->processManager;
    for (size_t i = 0; i < manager->GetCapacity(); ++i)
    {
        process_t* process = &manager->GetProcesses()[i];
        if (process->allocated && __atomic_load_n(&process->exited, __ATOMIC_ACQUIRE))
            (void) KillProcess(i, 0, true);
    }
}

[[noreturn]] void Scheduler::ExitCurrentTask()
{
    const uint8_t cpuId = arch_cpu_id();
    TaskManager* tasks = ResourceLayerImportCaps->taskManager;
    cpu_preemption_disable();
    if (IdleTasks[cpuId] == nullptr)
        panic();
    tasks->MarkDying(tasks->GetCurrentTask());
    (void) ResourceLayerImportCaps->processManager->SetRunningProcess(cpuId, nullptr);
    platform.cpus[cpuId].address_space = IdleAddressSpace;
    vmm_switch_addr_space(IdleAddressSpace);
    (void) tasks->ExecuteTask(IdleTasks[cpuId]);
    panic();
    __builtin_unreachable();
}

uint64_t Scheduler::SleepUntil(uint64_t monotonicDeadline)
{
    uint64_t now;
    if (!arch_time_read_monotonic(&now))
        return static_cast<uint64_t>(-38);
    if (monotonicDeadline <= now)
        return 0;
    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr || IdleAddressSpace == nullptr)
        return static_cast<uint64_t>(-38);
    const uint8_t cpuId = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
        return static_cast<uint64_t>(-22);
    ProcessManager* processManager = ResourceLayerImportCaps->processManager;
    TaskManager*    taskManager    = ResourceLayerImportCaps->taskManager;
    // This guard transfers with the suspended task; it does not guard the idle
    // task or its peers, and it never masks interrupts or holds a memory lock.
    cpu_preemption_disable();
    task_t*    task    = taskManager->GetCurrentTask();
    process_t* process = processManager->AcquireCurrentMemoryProcess(false);
    if (task == nullptr || process == nullptr || task->id >= SLEEP_QUEUE_CAPACITY)
    {
        if (process != nullptr)
            processManager->ReleaseMemoryProcess(process);
        cpu_preemption_enable();
        return static_cast<uint64_t>(-3);
    }
    if (IdleTasks[cpuId] == nullptr || !taskManager->MarkSleeping(task))
    {
        processManager->ReleaseMemoryProcess(process);
        cpu_preemption_enable();
        return static_cast<uint64_t>(-12);
    }
    LockQueues();
    sleep_entry_t& entry = SleepQueue[task->id];
    entry.process        = process;
    entry.task           = task;
    entry.deadline       = monotonicDeadline;
    entry.result         = 0;
    entry.cpuId          = cpuId;
    entry.waiting        = true;
    entry.dispatched     = false;
    size_t* link         = &SleepHeads[cpuId];
    while (*link != NO_SLEEPER && SleepQueue[*link].deadline <= monotonicDeadline)
        link = &SleepQueue[*link].next;
    entry.next = *link;
    *link      = task->id;
    // A blocked task is not a normal ready process. Runnable siblings may be
    // enqueued again by the scheduler independently of this sleeping task.
    for (uint8_t queuedCpu = 0; queuedCpu < BOOT_SMP_MAX_CPUS; ++queuedCpu)
        RemoveProcessFromQueue(&ReadyQueues[queuedCpu], process->id);
    UnlockQueues();
    (void) EnqueueProcess(cpuId, process->id);

    processManager->SetRunningProcess(cpuId, nullptr);
    platform.cpus[cpuId].address_space = IdleAddressSpace;
    vmm_switch_addr_space(platform.cpus[cpuId].address_space);
    bool switched = taskManager->ExecuteTask(IdleTasks[cpuId]);
    // A wake switches to this exact task and restores its guarded syscall frame.
    processManager->ActivateProcessAddressSpace(process);
    LockQueues();
    if (entry.waiting)
    {
        link = &SleepHeads[cpuId];
        while (*link != NO_SLEEPER && *link != task->id)
            link = &SleepQueue[*link].next;
        if (*link == task->id)
            *link = entry.next;
    }
    uint64_t result = switched ? entry.result : static_cast<uint64_t>(-3);
    entry.task      = nullptr;
    entry.process   = nullptr;
    UnlockQueues();
    taskManager->FinishSleeping(task);
    processManager->ReleaseMemoryProcess(process);
    cpu_preemption_enable();
    return result;
}

void Scheduler::ActivateScheduling()
{
    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        SchedulingActive[i] = true;
    }
}

bool Scheduler::IsSchedulingActive() const
{
    const uint8_t cpuId = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    return SchedulingActive[cpuId];
}

void Scheduler::InitializeBspProcessAndTask()
{
    if (ResourceLayerImportCaps == nullptr)
    {
        return;
    }

    if (ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr)
    {
        return;
    }

    ProcessManager* processManager = ResourceLayerImportCaps->processManager;
    TaskManager*    taskManager    = ResourceLayerImportCaps->taskManager;

    uint8_t cpuId = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return;
    }

    process_t* runningProcess = processManager->GetRunningProcess(cpuId);
    task_t*    runningTask    = taskManager->GetRunningTask(cpuId);
    if (runningProcess != nullptr && runningTask != nullptr)
    {
        return;
    }

    bool       createdProcess = false;
    process_t* process        = runningProcess;
    if (process == nullptr)
    {
        virt_addr_space_t* currentSpace = platform.cpus[cpuId].address_space;
        if (currentSpace == nullptr)
        {
            return;
        }

        process = processManager->CreateProcess(currentSpace);
        if (process == nullptr)
        {
            return;
        }

        createdProcess = true;
    }

    task_t* task = runningTask;
    if (task == nullptr)
    {
        task = taskManager->AllocateTask();
        if (task == nullptr)
        {
            if (createdProcess)
            {
                processManager->FreeProcess(process);
            }
            return;
        }

        if (!processManager->AddTask(process, task))
        {
            taskManager->FreeTask(task);
            if (createdProcess)
            {
                processManager->FreeProcess(process);
            }
            return;
        }
    }

    if (!processManager->SetRunningProcess(cpuId, process) || !taskManager->SetRunningTask(cpuId, task))
    {
        if (runningTask == nullptr)
        {
            taskManager->FreeTask(task);
        }
        if (createdProcess)
        {
            processManager->FreeProcess(process);
        }
    }
}

bool Scheduler::RemoveProcessFromQueue(ready_queue_t* queue, uint64_t processId)
{
    if (queue == nullptr || queue->count == 0)
    {
        return false;
    }

    bool     removed = false;
    uint64_t saved[READY_QUEUE_CAPACITY];
    size_t   original = queue->count;
    size_t   readPos  = queue->head;
    for (size_t i = 0; i < original; ++i)
    {
        saved[i] = queue->processIds[readPos];
        readPos  = (readPos + 1) % READY_QUEUE_CAPACITY;
    }

    queue->head  = 0;
    queue->tail  = 0;
    queue->count = 0;

    for (size_t i = 0; i < original; ++i)
    {
        const uint64_t queuedProcess = saved[i];

        if (queuedProcess == processId)
        {
            removed = true;
            continue;
        }

        queue->processIds[queue->tail] = queuedProcess;
        queue->tail                    = (queue->tail + 1) % READY_QUEUE_CAPACITY;
        queue->count++;
    }

    return removed;
}

bool Scheduler::ScheduleProcess(uint64_t processId)
{
    if (ResourceLayerImportCaps == nullptr)
    {
        return false;
    }

    if (ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr)
    {
        return false;
    }

    ProcessManager* processManager = ResourceLayerImportCaps->processManager;

    size_t count = processManager->GetCapacity();
    if (processId >= count)
    {
        return false;
    }

    process_t* table   = processManager->GetProcesses();
    process_t* process = &table[processId];
    if (!process->allocated || process->exited)
    {
        return false;
    }

    return ScheduleTask(process, nullptr);
}

bool Scheduler::ScheduleTask(process_t* process, task_t* task)
{
    ProcessManager* processManager = ResourceLayerImportCaps->processManager;
    TaskManager*    taskManager    = ResourceLayerImportCaps->taskManager;
    uint8_t         cpuId          = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    cpu_preemption_disable();
    LockQueues();
    if (!process->allocated || (process->exited && (task == nullptr || !task->sleepPending)))
    {
        UnlockQueues();
        cpu_preemption_enable();
        return false;
    }
    if (task == nullptr)
    {
        task = processManager->GetTasks(process);
        while (task != nullptr && task->sleepPending)
            task = task->next;
    }
    if (task == nullptr || !taskManager->RetainForDispatch(task))
    {
        UnlockQueues();
        cpu_preemption_enable();
        return false;
    }
    process_t*         previousProcess      = processManager->GetRunningProcess(cpuId);
    virt_addr_space_t* previousAddressSpace = platform.cpus[cpuId].address_space;

    if (!processManager->ActivateProcessAddressSpace(process))
    {
        taskManager->ReleaseDispatchClaim(task);
        UnlockQueues();
        cpu_preemption_enable();
        return false;
    }

    UnlockQueues();
    bool executed = taskManager->ExecuteTask(task, true);

    if (processManager->GetRunningProcess(cpuId) != previousProcess)
    {
        if (previousProcess != nullptr)
            (void) processManager->ActivateProcessAddressSpace(previousProcess);
        else
            (void) processManager->SetRunningProcess(cpuId, nullptr);
    }

    if (platform.cpus[cpuId].address_space != previousAddressSpace)
    {
        platform.cpus[cpuId].address_space = previousAddressSpace;
        if (previousAddressSpace != nullptr)
        {
            vmm_switch_addr_space(previousAddressSpace);
        }
    }

    cpu_preemption_enable();
    return executed;
}

bool Scheduler::EnqueueProcess(uint8_t cpuId, uint64_t processId)
{
    if (ResourceLayerImportCaps == nullptr)
    {
        return false;
    }

    if (ResourceLayerImportCaps->processManager == nullptr)
    {
        return false;
    }

    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    ProcessManager* processManager = ResourceLayerImportCaps->processManager;
    size_t          count          = processManager->GetCapacity();
    if (processId >= count)
    {
        return false;
    }

    process_t* table   = processManager->GetProcesses();
    process_t* process = &table[processId];
    LockQueues();
    if (!process->allocated || process->exited || (process->ownerCpu >= 0 && process->ownerCpu != cpuId))
    {
        UnlockQueues();
        return false;
    }

    task_t* firstTask = processManager->GetTasks(process);
    while (firstTask != nullptr && __atomic_load_n(&firstTask->sleepPending, __ATOMIC_ACQUIRE))
        firstTask = firstTask->next;
    if (firstTask == nullptr)
    {
        UnlockQueues();
        return false;
    }

    // Invariant: one process may be queued in at most one ready queue.
    // Suspended syscall contexts stay on their owner CPU, including runnable
    // siblings selected through the process queue.
    for (size_t i = 0; i < SLEEP_QUEUE_CAPACITY; ++i)
    {
        if (SleepQueue[i].process == process && SleepQueue[i].task != nullptr && SleepQueue[i].cpuId != cpuId)
        {
            UnlockQueues();
            return false;
        }
    }
    for (uint8_t otherCpu = 0; otherCpu < BOOT_SMP_MAX_CPUS; ++otherCpu)
    {
        ready_queue_t* otherQueue = &ReadyQueues[otherCpu];
        size_t         cursor     = otherQueue->head;
        for (size_t i = 0; i < otherQueue->count; ++i)
        {
            if (otherQueue->processIds[cursor] == processId)
            {
                UnlockQueues();
                return false;
            }
            cursor = (cursor + 1) % READY_QUEUE_CAPACITY;
        }
    }

    ready_queue_t* queue = &ReadyQueues[cpuId];
    if (queue->count >= READY_QUEUE_CAPACITY)
    {
        UnlockQueues();
        return false;
    }

    queue->processIds[queue->tail] = processId;
    queue->tail                    = (queue->tail + 1) % READY_QUEUE_CAPACITY;
    queue->count++;
    UnlockQueues();
    return true;
}

bool Scheduler::RunNextReadyProcess(uint8_t cpuId)
{
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    if (cpuId != arch_cpu_id())
    {
        return false;
    }

    cpu_preemption_disable();
    LockQueues();
    // Awakened tasks have their own slots, so a full process ready queue cannot
    // drop a wakeup. Resume the precise task that blocked inside the syscall.
    for (size_t i = 0; i < SLEEP_QUEUE_CAPACITY; ++i)
    {
        sleep_entry_t& entry = SleepQueue[i];
        if (entry.task != nullptr && entry.cpuId == cpuId && !entry.waiting && !entry.dispatched)
        {
            process_t* process = entry.process;
            task_t*    task    = entry.task;
            entry.dispatched   = true;
            UnlockQueues();
            bool executed = ScheduleTask(process, task);
            if (!executed)
            {
                LockQueues();
                entry.dispatched = false;
                UnlockQueues();
            }
            cpu_preemption_enable();
            return executed;
        }
    }
    ready_queue_t* queue = &ReadyQueues[cpuId];
    while (queue->count > 0)
    {
        const uint64_t processId = queue->processIds[queue->head];
        queue->head              = (queue->head + 1) % READY_QUEUE_CAPACITY;
        queue->count--;
        UnlockQueues();

        if (ScheduleProcess(processId))
        {
            cpu_preemption_enable();
            return true;
        }
        LockQueues();
    }

    UnlockQueues();
    cpu_preemption_enable();
    return false;
}

bool Scheduler::KillProcess(uint64_t processId, int32_t exitStatus, bool onlyIfExited)
{
    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr)
    {
        return false;
    }

    ProcessManager* processManager = ResourceLayerImportCaps->processManager;
    TaskManager*    taskManager    = ResourceLayerImportCaps->taskManager;
    const size_t    capacity       = processManager->GetCapacity();
    if (processId >= capacity)
    {
        return false;
    }

    process_t* table   = processManager->GetProcesses();
    process_t* process = &table[processId];
    LockQueues();
    if (!process->allocated || (onlyIfExited && !process->exited))
    {
        UnlockQueues();
        return false;
    }

    if (!onlyIfExited)
        process->exitStatus = exitStatus;
    __atomic_store_n(&process->exited, true, __ATOMIC_RELEASE);

    for (uint8_t cpu = 0; cpu < BOOT_SMP_MAX_CPUS; ++cpu)
    {
        RemoveProcessFromQueue(&ReadyQueues[cpu], processId);
    }
    // Retirement and dispatch claims share QueueLock.
    void* retiredStacks[TaskManager::MAX_TASKS];
    size_t retiredCount = 0;
    task_t* iter = process->tasks;
    while (iter != nullptr)
    {
        task_t* next = iter->next;

        task_t* previous = iter->prev;
        taskManager->MarkDying(iter);
        void* retired = nullptr;
        if (taskManager->FreeTask(iter, &retired))
        {
            if (retired != nullptr)
                retiredStacks[retiredCount++] = retired;
            if (previous != nullptr)
            {
                previous->next = next;
            }
            else
            {
                process->tasks = next;
            }

            if (next != nullptr)
            {
                next->prev = previous;
            }
        }

        iter = next;
    }

    const bool reap = process->tasks == nullptr;
    UnlockQueues();
    // No scheduler lock spans unmapping or synchronous shootdown. CPUs in a
    // timer handler can finish their short queue transaction and accept IPIs.
    for (size_t i = 0; i < retiredCount; ++i)
        TaskManager::ReleaseTaskStack(retiredStacks[i]);
    if (reap)
        (void) processManager->TryReapExitedProcess(processId);

    return true;
}
