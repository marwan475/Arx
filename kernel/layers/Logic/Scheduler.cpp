#include "layers/Logic/Scheduler.hpp"

#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/TimeManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <platform.h>
}

Scheduler::Scheduler(ResourceLayerCaps* resourceLayerCaps)
{
    ResourceLayerImportCaps = resourceLayerCaps;
    Events                  = nullptr;
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
        {
            panic();
        }
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
    {
        return;
    }

    uint64_t now            = 0;
    bool     clockAvailable = arch_time_read_monotonic(&now);

    if (clockAvailable && ResourceLayerImportCaps->timeManager != nullptr)
    {
        ProcessManager* processManager = ResourceLayerImportCaps->processManager;

        for (uint64_t id = 0; id < processManager->GetCapacity(); ++id)
        {
            process_t* process = processManager->AcquireProcess(id, true);

            if (process == nullptr)
            {
                continue;
            }

            if (ResourceLayerImportCaps->timeManager->ExpireRealTimer(process, now))
            {
                WakeSignalWaiters(process);
            }

            processManager->ReleaseProcess(process);
        }
    }

    LockQueues();
    size_t* link = &SleepHeads[cpuId];

    while (*link != NO_SLEEPER)
    {
        sleep_entry_t& entry       = SleepQueue[*link];
        const bool     exited      = __atomic_load_n(&entry.process->exited, __ATOMIC_ACQUIRE) || entry.task->dying;
        const bool     changed     = entry.sequence != nullptr && __atomic_load_n(entry.sequence, __ATOMIC_ACQUIRE) != entry.expectedSequence;
        const bool     timed       = entry.deadline != UINT64_MAX;
        const bool     interrupted = !entry.uninterruptible && pending_signals(entry.process, entry.task) != 0;

        if ((timed && !clockAvailable) || exited || interrupted || changed || (timed && entry.deadline <= now))
        {
            *link         = entry.next;
            entry.next    = NO_SLEEPER;
            entry.waiting = false;
            entry.result  = exited ? static_cast<uint64_t>(-3) : interrupted ? static_cast<uint64_t>(-4) : timed && !clockAvailable && !changed ? static_cast<uint64_t>(-38) : 0;
            __atomic_store_n(&entry.task->sleepBlocked, false, __ATOMIC_RELEASE);
        }
        else
            link = &entry.next;
    }

    UnlockQueues();
}

void Scheduler::WakeSignalWaiters(process_t* process)
{
    LockQueues();

    for (uint8_t cpu = 0; cpu < BOOT_SMP_MAX_CPUS; ++cpu)
    {
        size_t* link = &SleepHeads[cpu];

        while (*link != NO_SLEEPER)
        {
            sleep_entry_t& entry = SleepQueue[*link];

            if (entry.process == process && !entry.uninterruptible && pending_signals(process, entry.task) != 0)
            {
                *link         = entry.next;
                entry.next    = NO_SLEEPER;
                entry.waiting = false;
                entry.result  = (uint64_t) -4;
                __atomic_store_n(&entry.task->sleepBlocked, false, __ATOMIC_RELEASE);
            }
            else
            {
                link = &entry.next;
            }
        }
    }

    UnlockQueues();
    if (Events != nullptr)
    {
        Events->NotifySignals(process);
    }
}

void Scheduler::ReapExitedProcesses()
{
    ProcessManager* manager = ResourceLayerImportCaps->processManager;

    for (size_t i = 0; i < manager->GetCapacity(); ++i)
    {
        process_t* process = &manager->GetProcesses()[i];

        if (process->allocated && __atomic_load_n(&process->exited, __ATOMIC_ACQUIRE))
        {
            (void) KillProcess(i, 0, true);
        }
        else if (process->allocated)
        {
            TaskManager* tasks = ResourceLayerImportCaps->taskManager;

            for (task_t* task = process->tasks; task != nullptr;)
            {
                task_t* next = task->next;

                if (tasks->CleanupRetiredTask(task))
                {
                    LockQueues();
                    task_t*                  previous         = task->prev;
                    void*                    retired          = nullptr;
                    retired_task_resources_t retiredResources = {};

                    if (tasks->FreeTask(task, &retired, &retiredResources))
                    {
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

                    UnlockQueues();
                    TaskManager::ReleaseTaskResources(retiredResources);
                    TaskManager::ReleaseTaskStack(retired);
                }

                task = next;
            }
        }
    }
}

[[noreturn]] void Scheduler::ExitCurrentTask()
{
    const uint8_t cpuId = arch_cpu_id();
    TaskManager*  tasks = ResourceLayerImportCaps->taskManager;
    cpu_preemption_disable();

    if (IdleTasks[cpuId] == nullptr)
    {
        panic();
    }

    tasks->CleanupCurrentTask();
    tasks->MarkDying(tasks->GetCurrentTask());
    process_t* process = ResourceLayerImportCaps->processManager->GetCurrentProcess();

    if (process != nullptr && !process->exited)
    {
        __atomic_add_fetch(&process->childSequence, 1ULL, __ATOMIC_RELEASE);
        WakeEventWaiters(&process->childSequence);
        (void) EnqueueProcess(cpuId, process->id);
    }
    (void) ResourceLayerImportCaps->processManager->SetRunningProcess(cpuId, nullptr);
    platform.cpus[cpuId].address_space = IdleAddressSpace;
    vmm_switch_addr_space(IdleAddressSpace);
    (void) tasks->ExecuteTask(IdleTasks[cpuId]);
    panic();
    __builtin_unreachable();
}

uint64_t Scheduler::SleepUntil(uint64_t monotonicDeadline)
{
    return Sleep(monotonicDeadline, nullptr, 0);
}

uint64_t Scheduler::WaitForEvent(const uint64_t* sequence, uint64_t expected, uint64_t monotonicDeadline, bool uninterruptible)
{
    if (sequence == nullptr)
    {
        return static_cast<uint64_t>(-22);
    }

    return Sleep(monotonicDeadline, sequence, expected, uninterruptible);
}

void Scheduler::WakeEventWaiters(const uint64_t* sequence)
{
    if (sequence == nullptr)
    {
        return;
    }

    LockQueues();

    for (size_t cpu = 0; cpu < BOOT_SMP_MAX_CPUS; ++cpu)
    {
        size_t* link = &SleepHeads[cpu];

        while (*link != NO_SLEEPER)
        {
            sleep_entry_t& entry = SleepQueue[*link];

            if (entry.sequence == sequence && __atomic_load_n(sequence, __ATOMIC_ACQUIRE) != entry.expectedSequence)
            {
                *link         = entry.next;
                entry.next    = NO_SLEEPER;
                entry.waiting = false;
                entry.result  = 0;
                __atomic_store_n(&entry.task->sleepBlocked, false, __ATOMIC_RELEASE);
            }
            else
                link = &entry.next;
        }
    }

    UnlockQueues();
}

uint64_t Scheduler::Sleep(uint64_t monotonicDeadline, const uint64_t* sequence, uint64_t expected, bool uninterruptible)
{
    if (sequence != nullptr && __atomic_load_n(sequence, __ATOMIC_ACQUIRE) != expected)
    {
        return 0;
    }

    uint64_t now = 0;

    if (monotonicDeadline != UINT64_MAX && !arch_time_read_monotonic(&now))
    {
        return static_cast<uint64_t>(-38);
    }

    if (monotonicDeadline <= now)
    {
        return 0;
    }

    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr || IdleAddressSpace == nullptr)
    {
        return static_cast<uint64_t>(-38);
    }

    const uint8_t cpuId = arch_cpu_id();

    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return static_cast<uint64_t>(-22);
    }

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
        {
            processManager->ReleaseMemoryProcess(process);
        }

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
    // Close the readiness-scan/parking race under the wakeup queue lock.

    if (!uninterruptible && pending_signals(process, task) != 0)
    {
        UnlockQueues();
        taskManager->FinishSleeping(task);
        processManager->ReleaseMemoryProcess(process);
        cpu_preemption_enable();
        return (uint64_t) -4;
    }

    if (sequence != nullptr && __atomic_load_n(sequence, __ATOMIC_ACQUIRE) != expected)
    {
        UnlockQueues();
        taskManager->FinishSleeping(task);
        processManager->ReleaseMemoryProcess(process);
        cpu_preemption_enable();
        return 0;
    }

    const size_t taskIndex = static_cast<size_t>(task - taskManager->GetTasks());
    sleep_entry_t& entry   = SleepQueue[taskIndex];
    entry.process          = process;
    entry.task             = task;
    entry.deadline         = monotonicDeadline;
    entry.result           = 0;
    entry.sequence         = sequence;
    entry.expectedSequence = expected;
    entry.cpuId            = cpuId;
    entry.waiting          = true;
    entry.dispatched       = false;
    entry.uninterruptible  = uninterruptible;
    size_t* link           = &SleepHeads[cpuId];

    while (*link != NO_SLEEPER && SleepQueue[*link].deadline <= monotonicDeadline)
    {
        link = &SleepQueue[*link].next;
    }

    entry.next = *link;
    *link      = taskIndex;
    // A blocked task is not a normal ready process. Runnable siblings may be
    // enqueued again by the scheduler independently of this sleeping task.

    for (uint8_t queuedCpu = 0; queuedCpu < BOOT_SMP_MAX_CPUS; ++queuedCpu)
    {
        RemoveProcessFromQueue(&ReadyQueues[queuedCpu], process->id);
    }

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

        while (*link != NO_SLEEPER && *link != taskIndex)
        {
            link = &SleepQueue[*link].next;
        }

        if (*link == taskIndex)
        {
            *link = entry.next;
        }
    }

    uint64_t result = switched ? entry.result : static_cast<uint64_t>(-3);
    entry.task      = nullptr;
    entry.process   = nullptr;
    entry.sequence  = nullptr;
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

    if (!process->allocated || (process->stopped && !process->exited) || (process->exited && (task == nullptr || !task->sleepPending)))
    {
        UnlockQueues();
        cpu_preemption_enable();
        return false;
    }

    if (task == nullptr)
    {
        task_t* first = nullptr;

        for (task_t* candidate = processManager->GetTasks(process); candidate != nullptr; candidate = candidate->next)
        {
            if (!candidate->allocated || candidate->dying || candidate->sleepPending || candidate->state == task_t::SWITCHING)
            {
                continue;
            }

            if (first == nullptr || candidate->id < first->id)
            {
                first = candidate;
            }

            if (candidate->id >= process->taskCursor && (task == nullptr || candidate->id < task->id))
            {
                task = candidate;
            }
        }

        task = task != nullptr ? task : first;

        if (task != nullptr)
        {
            process->taskCursor = task->id + 1;
        }
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
        {
            (void) processManager->ActivateProcessAddressSpace(previousProcess);
        }
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

    while (firstTask != nullptr && (firstTask->dying || __atomic_load_n(&firstTask->sleepPending, __ATOMIC_ACQUIRE)))
    {
        firstTask = firstTask->next;
    }

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
                // Timer dispatch may already have queued this process. Its
                // existing local entry will also dispatch newly added siblings.
                return otherCpu == cpuId;
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

bool Scheduler::PublishTask(process_t* process, task_t* task)
{
    cpu_preemption_disable();
    LockQueues();
    const bool attached = process->allocated && !process->exited && ResourceLayerImportCaps->processManager->AddTask(process, task);
    UnlockQueues();
    const bool queued = attached && EnqueueProcess(arch_cpu_id(), process->id);

    if (attached && !queued)
    {
        LockQueues();
        ResourceLayerImportCaps->processManager->RemoveTask(process, task);
        UnlockQueues();
    }

    cpu_preemption_enable();
    return queued;
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

    process_t* process = processManager->AcquireProcess(processId);

    if (process == nullptr)
    {
        return false;
    }

    LockQueues();

    if (process->retirementActive || (onlyIfExited && !process->exited))
    {
        UnlockQueues();
        processManager->ReleaseProcess(process);
        return false;
    }

    process->retirementActive = true;

    if (!onlyIfExited)
    {
        process->exitStatus = exitStatus;
    }

    __atomic_store_n(&process->exited, true, __ATOMIC_RELEASE);

    for (uint8_t cpu = 0; cpu < BOOT_SMP_MAX_CPUS; ++cpu)
    {
        RemoveProcessFromQueue(&ReadyQueues[cpu], processId);
    }

    // Mark before unlocking so new dispatch claims cannot start. Cleanup may
    // touch user memory and wake futexes; no scheduler lock may span it.

    for (task_t* task = process->tasks; task != nullptr; task = task->next)
    {
        taskManager->MarkDying(task);
    }

    UnlockQueues();
    NotifyProcessExit(process);
    void*   retiredStacks[TaskManager::MAX_TASKS];
    size_t  retiredCount = 0;
    task_t* iter         = process->tasks;

    while (iter != nullptr)
    {
        task_t* next = iter->next;

        task_t*                  previous         = iter->prev;
        void*                    retired          = nullptr;
        retired_task_resources_t retiredResources = {};
        // Run callbacks before taking QueueLock, then retire and unlink under
        // the same lock used when attaching a newly allocated task slot.
        const bool cleaned = taskManager->CleanupRetiredTask(iter);
        LockQueues();

        if (cleaned && taskManager->FreeTask(iter, &retired, &retiredResources))
        {
            if (retired != nullptr)
            {
                retiredStacks[retiredCount++] = retired;
            }

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

        UnlockQueues();
        TaskManager::ReleaseTaskResources(retiredResources);

        iter = next;
    }

    LockQueues();
    const bool reap           = process->tasks == nullptr && (!process->waitable || process->waitConsumed);
    process->retirementActive = false;
    UnlockQueues();
    // No scheduler lock spans unmapping or synchronous shootdown. CPUs in a
    // timer handler can finish their short queue transaction and accept IPIs.

    for (size_t i = 0; i < retiredCount; ++i)
    {
        TaskManager::ReleaseTaskStack(retiredStacks[i]);
    }

    if (process->tasks == nullptr)
    {
        ProcessManager::ReleaseProcessResources(process);
    }

    processManager->ReleaseProcess(process);

    if (reap)
    {
        (void) processManager->TryReapExitedProcess(processId);
    }

    return true;
}

void Scheduler::NotifyProcessExit(process_t* process)
{
    bool expected = false;

    if (!__atomic_compare_exchange_n(&process->exitNotified, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        return;
    }

    ProcessManager* manager = ResourceLayerImportCaps->processManager;
    process_t* parent       = process->hasParent ? manager->AcquireProcess(process->parentId) : nullptr;

    if (parent != nullptr && !parent->exited)
    {
        SignalDispositionGuard signalGuard(parent);
        const signal_action_t  action = parent->Signals()->actions[16];

        if (process->exitSignal == 17 && (action.handler == 1 || (action.flags & 2) != 0))
        {
            process->waitConsumed = true;
        }

        if (process->exitSignal > 0 && process->exitSignal < 32 && (parent->Signals()->ignored & (1ULL << (process->exitSignal - 1))) == 0)
        {
            parent->signalSenders[process->exitSignal - 1] = {static_cast<int32_t>(process->id), process->credentials.user.real, 1};
            __atomic_fetch_or(&parent->signalPending, 1ULL << (process->exitSignal - 1), __ATOMIC_RELEASE);
        }

        signalGuard.Release();
        __atomic_add_fetch(&parent->childSequence, 1ULL, __ATOMIC_RELEASE);
        WakeEventWaiters(&parent->childSequence);
        WakeSignalWaiters(parent);
    }
    else
    {
        process->waitConsumed = true;
    }

    if (parent != nullptr)
    {
        manager->ReleaseProcess(parent);
    }

    // PID 1 adopts orphans when present; otherwise the kernel reaps them.
    process_t* adopter = manager->AcquireProcess(1);
    bool userInit = false;

    for (task_t* task = adopter != nullptr ? adopter->tasks : nullptr; task != nullptr; task = task->next)
    {
        userInit = userInit || (task->allocated && task->isUserTask && !task->dying);
    }

    for (size_t index = 0; index < manager->GetCapacity(); ++index)
    {
        process_t* child = manager->AcquireProcess(index);

        if (child != nullptr)
        {
            if (child->hasParent && child->parentId == process->id)
            {
                child->hasParent = userInit && adopter != process && !adopter->exited;
                child->parentId  = child->hasParent ? adopter->id : 0;
                child->parentTaskId = child->hasParent ? adopter->id : 0;

                if (!child->hasParent)
                {
                    child->waitConsumed = true;
                }
                else
                {
                    __atomic_add_fetch(&adopter->childSequence, 1ULL, __ATOMIC_RELEASE);
                    WakeEventWaiters(&adopter->childSequence);
                }
            }

            manager->ReleaseProcess(child);
        }
    }

    if (adopter != nullptr)
    {
        manager->ReleaseProcess(adopter);
    }

    __atomic_add_fetch(&process->execSequence, 1ULL, __ATOMIC_RELEASE);
    WakeEventWaiters(&process->execSequence);
}

void Scheduler::ChangeStoppedState(process_t* process, int signal, bool stopped)
{
    SignalDispositionGuard signalGuard(process);
    const bool             changed = process->stopped != stopped;
    process->stopped               = stopped;

    if (changed)
    {
        process->stopSignal       = stopped ? signal : process->stopSignal;
        process->stopPending      = stopped;
        process->continuedPending = !stopped;
    }

    signalGuard.Release();
    __atomic_add_fetch(&process->stopSequence, 1ULL, __ATOMIC_RELEASE);
    WakeEventWaiters(&process->stopSequence);

    if (!stopped)
    {
        const uint8_t cpu = process->ownerCpu >= 0 ? static_cast<uint8_t>(process->ownerCpu) : arch_cpu_id();
        (void) EnqueueProcess(cpu, process->id);
    }

    ProcessManager* manager = ResourceLayerImportCaps->processManager;
    process_t* parent       = changed && process->hasParent ? manager->AcquireProcess(process->parentId) : nullptr;

    if (parent != nullptr)
    {
        SignalDispositionGuard signalGuard(parent);

        if ((parent->Signals()->actions[16].flags & 1) == 0 && (parent->Signals()->ignored & (1ULL << 16)) == 0)
        {
            parent->signalSenders[16] = {static_cast<int32_t>(process->id), process->credentials.user.real, stopped ? 5 : 6};
            __atomic_fetch_or(&parent->signalPending, 1ULL << 16, __ATOMIC_RELEASE);
        }

        signalGuard.Release();
        __atomic_add_fetch(&parent->childSequence, 1ULL, __ATOMIC_RELEASE);
        WakeEventWaiters(&parent->childSequence);
        WakeSignalWaiters(parent);
        manager->ReleaseProcess(parent);
    }
}

void Scheduler::SetEventManager(EventManager* events)
{
    Events = events;
}
