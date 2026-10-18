#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/SignalManager.hpp"

#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/SynchronizationManager.hpp"
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
    PriorityManager         = nullptr;
    QueueLock               = 0;
    ReadySequence = 0;
    memset(DeadlineUtilization, 0, sizeof(DeadlineUtilization));
    memset(RealtimeWindow, 0, sizeof(RealtimeWindow));
    memset(RealtimeConsumed, 0, sizeof(RealtimeConsumed));
    memset(ExecutionStamp, 0, sizeof(ExecutionStamp));
    memset(ExecutionTask, 0, sizeof(ExecutionTask));
    IdleAddressSpace        = platform.cpus[platform.bsp_id].address_space;
    memset(SleepQueue, 0, sizeof(SleepQueue));
    static_assert(SLEEP_QUEUE_CAPACITY == TaskManager::MAX_TASKS, "one sleep entry per task");

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
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

        // Idle contexts belong to one CPU. Priority refresh also visits tasks
        // without a process and must not place all idle stacks on the BSP.
        IdleTasks[i]->affinityMask = 1ULL << i;
        IdleTasks[i]->placementCpu = i;
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
            arch_idle_wait();
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

    if (clockAvailable)
    {
        uint64_t loads[3];
        uint16_t live;
        ResourceLayerImportCaps->taskManager->ReadSystemStatistics(now, loads, &live);
    }

    if (clockAvailable && ResourceLayerImportCaps->timeManager != nullptr)
    {
        // Timer probes retain process slots only for this short pass. Do not
        // suspend an idle task with a transient reference to another process.
        cpu_preemption_disable();
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
        cpu_preemption_enable();
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
        {
            link = &entry.next;
        }
    }

    UnlockQueues();
}

void Scheduler::AccountCurrent(bool userMode)
{
    TaskManager* tasks = ResourceLayerImportCaps->taskManager;
    tasks->AccountCurrent(false);
    task_t* task = tasks->GetCurrentTask();

    // Interrupts may sample time inside a guarded operation, but must not
    // reacquire a timer/disposition lock interrupted on this CPU.
    if (platform.cpus[arch_cpu_id()].preemption_depth == 0 && task != nullptr && task->isUserTask && ResourceLayerImportCaps->timeManager != nullptr)
    {
        if (ResourceLayerImportCaps->timeManager->ExpireCpuTimers(task->process))
        {
            WakeSignalWaiters(task->process);
        }
    }

    tasks->AccountCurrent(userMode);
}

void Scheduler::WakeSignalWaiters(process_t* process)
{
    uint64_t reschedule = 0;
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
                reschedule |= 1ULL << (entry.task->placementCpu >= 0 ? entry.task->placementCpu : entry.cpuId);
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
    RequestReschedule(reschedule);
    if (Events != nullptr)
    {
        Events->NotifySignals(process);
    }
}

void Scheduler::ReapExitedProcesses()
{
    // A suspended reaper must not retain a process or block another CPU's
    // retirement pass while the scheduler runs a different task.
    cpu_preemption_disable();
    ProcessManager* manager = ResourceLayerImportCaps->processManager;

    for (size_t i = 0; i < manager->GetCapacity(); ++i)
    {
        process_t* process = manager->AcquireProcess(i);

        if (process == nullptr)
        {
            continue;
        }

        LockQueues();
        const bool exited      = __atomic_load_n(&process->exited, __ATOMIC_ACQUIRE);
        const bool retireTasks = !exited && !process->retirementActive;

        if (retireTasks)
        {
            process->retirementActive = true;
        }

        UnlockQueues();

        if (exited)
        {
            manager->ReleaseProcess(process);
            (void) KillProcess(i, 0, true);
            continue;
        }

        if (retireTasks)
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

            LockQueues();
            process->retirementActive = false;
            UnlockQueues();
        }

        manager->ReleaseProcess(process);
    }

    cpu_preemption_enable();
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

    tasks->MarkDying(tasks->GetCurrentTask());
    tasks->CleanupCurrentTask();
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

    uint64_t reschedule = 0;
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
                reschedule |= 1ULL << (entry.task->placementCpu >= 0 ? entry.task->placementCpu : entry.cpuId);
                entry.result  = 0;
                __atomic_store_n(&entry.task->sleepBlocked, false, __ATOMIC_RELEASE);
            }
            else
            {
                link = &entry.next;
            }
        }
    }

    UnlockQueues();
    RequestReschedule(reschedule);
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
    task->scheduling->readyOrder = 0;
    entry.deadline         = monotonicDeadline;
    entry.result           = 0;
    entry.sequence         = sequence;
    entry.expectedSequence = expected;
    entry.cpuId            = cpuId;
    entry.waiting          = true;
    entry.dispatched       = false;
    entry.uninterruptible  = uninterruptible;
    __atomic_store_n(&task->sleepUninterruptible, uninterruptible, __ATOMIC_RELEASE);
    size_t* link           = &SleepHeads[cpuId];

    while (*link != NO_SLEEPER && SleepQueue[*link].deadline <= monotonicDeadline)
    {
        link = &SleepQueue[*link].next;
    }

    entry.next = *link;
    *link      = taskIndex;
    // The sleep slot retains this exact task; runnable siblings keep their
    // independent published task slots across CPU placement changes.

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
        link = &SleepHeads[entry.cpuId];

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

bool Scheduler::ScheduleTask(process_t* process, task_t* task, bool dispatchClaim)
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
        if (dispatchClaim)
        {
            taskManager->ReleaseDispatchClaim(task);
        }
        UnlockQueues();
        cpu_preemption_enable();
        return false;
    }

    if (task == nullptr)
    {
        task_t* first = nullptr;

        for (task_t* candidate = processManager->GetTasks(process); candidate != nullptr; candidate = candidate->next)
        {
            if (!candidate->allocated || candidate->dying || candidate->sleepPending || candidate->state == task_t::SWITCHING || !(candidate->affinityMask & (1ULL << cpuId)) || (candidate->placementCpu >= 0 && candidate->placementCpu != cpuId)
                || (candidate->state == task_t::RUNNING && taskManager->GetRunningTask(cpuId) != candidate))
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

    if (task == nullptr || (!dispatchClaim && !taskManager->RetainForDispatch(task)))
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
    cpuId         = arch_cpu_id();

    if (processManager->GetRunningProcess(cpuId) != previousProcess)
    {
        if (previousProcess != nullptr)
        {
            (void) processManager->ActivateProcessAddressSpace(previousProcess);
        }
        else
        {
            (void) processManager->SetRunningProcess(cpuId, nullptr);
        }
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

uint64_t Scheduler::AvailableCpuMask()
{
    const size_t count = platform.cpu_count < BOOT_SMP_MAX_CPUS ? platform.cpu_count : BOOT_SMP_MAX_CPUS;
    return count >= 64 ? UINT64_MAX : (1ULL << count) - 1;
}

uint64_t Scheduler::SetNice(task_t* task, int32_t nice)
{
    if (task == nullptr || nice < -20 || nice > 19)
    {
        return LINUX_EINVAL;
    }

    LockQueues();
    __atomic_store_n(&task->baseNice, nice, __ATOMIC_RELEASE);
    UnlockQueues();
    if (PriorityManager != nullptr)
    {
        PriorityManager->RefreshPriorities();
    }
    else
    {
        __atomic_store_n(&task->effectiveNice, nice, __ATOMIC_RELEASE);
    }
    return 0;
}

uint64_t Scheduler::SetAffinity(task_t* task, uint64_t mask)
{
    if (!arch_task_context_supported())
    {
        return LINUX_EOPNOTSUPP;
    }
    mask &= AvailableCpuMask();

    if (task == nullptr || mask == 0)
    {
        return LINUX_EINVAL;
    }

    if (PriorityManager != nullptr && !PriorityManager->LockSchedulingChange(task, mask))
    {
        return static_cast<uint64_t>(-16);
    }

    LockQueues();
    auto& policy = *task->scheduling;
    int reservation = policy.reservationCpu;

    if (policy.policy == SCHEDULING_DEADLINE)
    {
        const uint64_t utilization = ReservationUtilization(policy.deadlineRuntime, policy.deadlinePeriod);

        if (reservation < 0 || !(mask & (1ULL << reservation)))
        {
            reservation = FindReservationCpuLocked(task, mask, utilization);

            if (reservation < 0)
            {
                UnlockQueues();

                if (PriorityManager != nullptr)
                {
                    PriorityManager->UnlockSchedulingChange();
                }

                return static_cast<uint64_t>(-16);
            }

            DeadlineUtilization[policy.reservationCpu] -= utilization;
            DeadlineUtilization[reservation] += utilization;
            policy.reservationCpu = reservation;
        }
    }
    else if (policy.effectivePolicy == SCHEDULING_DEADLINE && policy.deadlineDonor != nullptr && !(mask & (1ULL << policy.deadlineDonor->scheduling->reservationCpu)))
    {
        UnlockQueues();

        if (PriorityManager != nullptr)
        {
            PriorityManager->UnlockSchedulingChange();
        }

        return static_cast<uint64_t>(-16);
    }

    __atomic_store_n(&task->affinityMask, mask, __ATOMIC_RELEASE);
    __atomic_store_n(&task->migrationEnabled, true, __ATOMIC_RELEASE);
    UnlockQueues();

    if (PriorityManager != nullptr)
    {
        PriorityManager->UnlockSchedulingChange();
    }

    (void) RunSchedulingAlgorithm(arch_cpu_id(), scheduling_reason_t::AFFINITY, nullptr, task);
    uint64_t reschedule = 1ULL << task->placementCpu;

    if (task->ownerCpu >= 0)
    {
        reschedule |= 1ULL << task->ownerCpu;
    }

    RequestReschedule(reschedule);

    // A running task must pass through a completed switch before another CPU
    // can claim its saved stack. Self migration parks on the CPU's idle stack.
    TaskManager* tasks   = ResourceLayerImportCaps->taskManager;
    task_t*      current = tasks->GetCurrentTask();

    if (current == task && (!(mask & (1ULL << arch_cpu_id())) || task->placementCpu != arch_cpu_id()))
    {
        cpu_preemption_disable();
        process_t* memoryProcess = ResourceLayerImportCaps->processManager->GetCurrentProcess();
        task->schedulerQueued    = true;
        ResourceLayerImportCaps->processManager->SetRunningProcess(arch_cpu_id(), nullptr);
        platform.cpus[arch_cpu_id()].address_space = IdleAddressSpace;
        vmm_switch_addr_space(IdleAddressSpace);
        (void) tasks->ExecuteTask(IdleTasks[arch_cpu_id()]);
        if (memoryProcess != nullptr)
        {
            ResourceLayerImportCaps->processManager->ActivateProcessAddressSpace(memoryProcess);
        }
        cpu_preemption_enable();
    }

    // Remote tasks observe the mask at dispatch. Wait until the old execution
    // has quiesced before reporting a completed restriction.
    for (;;)
    {
        bool outside = false;
        for (uint8_t cpu = 0; cpu < platform.cpu_count; ++cpu)
        {
            outside |= (!(mask & (1ULL << cpu)) || cpu != task->placementCpu) && tasks->GetRunningTask(cpu) == task;
        }
        if (!outside)
        {
            break;
        }
        arch_pause();
    }
    return 0;
}

bool Scheduler::EnqueueProcess(uint8_t cpuId, uint64_t processId)
{
    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->processManager == nullptr || processId >= ResourceLayerImportCaps->processManager->GetCapacity())
    {
        return false;
    }
    process_t* process = &ResourceLayerImportCaps->processManager->GetProcesses()[processId];
    const auto decision = RunSchedulingAlgorithm(cpuId, scheduling_reason_t::PUBLISH, process);
    return decision.accepted && decision.local;
}

bool Scheduler::PublishTask(process_t* process, task_t* task)
{
    cpu_preemption_disable();
    LockQueues();
    const bool attached = process->allocated && !process->exited && ResourceLayerImportCaps->processManager->AddTask(process, task);
    UnlockQueues();
    const bool queued = attached && RunSchedulingAlgorithm(arch_cpu_id(), scheduling_reason_t::PUBLISH, process).accepted;

    if (attached && !queued)
    {
        LockQueues();
        ResourceLayerImportCaps->processManager->RemoveTask(process, task);
        UnlockQueues();
    }

    cpu_preemption_enable();
    return queued;
}

Scheduler::scheduling_decision_t Scheduler::RunSchedulingAlgorithm(uint8_t cpuId, scheduling_reason_t reason, process_t* publish, task_t* placement)
{
    scheduling_decision_t decision = {nullptr, nullptr, UINT64_MAX, cpuId, false, false};

    if (cpuId >= platform.cpu_count)
    {
        return decision;
    }

    if (placement != nullptr)
    {
        LockQueues();
        const uint64_t mask = placement->affinityMask & AvailableCpuMask();
        const auto& policy = *placement->scheduling;
        task_t* donor = policy.deadlineDonor != nullptr ? policy.deadlineDonor : placement;
        const int reserved = policy.effectivePolicy == SCHEDULING_DEADLINE ? donor->scheduling->reservationCpu : -1;

        if (placement->allocated && !placement->dying && mask != 0 && (reserved < 0 || (mask & (1ULL << reserved))))
        {
            placement->placementCpu = reserved >= 0 ? reserved : placement->placementCpu >= 0 && (mask & (1ULL << placement->placementCpu)) ? placement->placementCpu : (mask & (1ULL << cpuId)) ? cpuId : __builtin_ctzll(mask);
            placement->migrationEnabled = true;
            decision.cpu = placement->placementCpu;
            decision.accepted = true;
            decision.local = decision.cpu == cpuId;
        }

        UnlockQueues();
        return decision;
    }

    if (publish != nullptr)
    {
        uint64_t remote = 0;
        LockQueues();

        if (publish->allocated && !publish->exited && publish->ownerCpu < static_cast<int16_t>(platform.cpu_count))
        {
            for (task_t* task = publish->tasks; task != nullptr; task = task->next)
            {
                if (!task->allocated || task->dying || task->sleepPending)
                {
                    continue;
                }

                const uint64_t mask = task->affinityMask & AvailableCpuMask();

                if (mask == 0)
                {
                    continue;
                }

                if (task->placementCpu < 0 || !(mask & (1ULL << task->placementCpu)))
                {
                    task->placementCpu = (mask & (1ULL << cpuId)) ? cpuId : __builtin_ctzll(mask);
                }

                task->schedulerQueued = true;

                if (task->scheduling->readyOrder == 0)
                {
                    task->scheduling->readyOrder = ++ReadySequence;
                }

                decision.accepted = true;
                decision.local |= task->placementCpu == cpuId;

                if (task->placementCpu != cpuId)
                {
                    remote |= 1ULL << task->placementCpu;
                }
            }
        }

        UnlockQueues();
        RequestReschedule(remote);
        return decision;
    }

    LockQueues();
    uint64_t now = 0;
    arch_time_read_monotonic(&now);
    AccountSchedulingLocked(cpuId, now);
    ReplenishReservationsLocked(now);
    task_t* current = ResourceLayerImportCaps->taskManager->GetRunningTask(cpuId);

    if (current != nullptr && ResourceLayerImportCaps->processManager->GetRunningProcess(cpuId) != nullptr && !current->dying && !current->sleepPending)
    {
        current->schedulerQueued = true;

        if (current->placementCpu < 0)
        {
            current->placementCpu = cpuId;
        }

        auto& policy = *current->scheduling;

        if (reason == scheduling_reason_t::YIELD)
        {
            policy.readyOrder = ++ReadySequence;
            policy.roundRobinRemaining = SCHEDULING_RR_QUANTUM;

            if (policy.policy == SCHEDULING_DEADLINE)
            {
                policy.deadlineRemaining = 0;
            }
        }

        if (policy.policy == SCHEDULING_RR && policy.roundRobinRemaining == 0)
        {
            policy.roundRobinRemaining = SCHEDULING_RR_QUANTUM;
            policy.readyOrder = ++ReadySequence;
        }
    }

    TaskManager*   tasks    = ResourceLayerImportCaps->taskManager;
    task_t*        selected = nullptr;
    sleep_entry_t* sleeper  = nullptr;

    for (size_t i = 0; i < TaskManager::MAX_TASKS; ++i)
    {
        task_t*        task  = &tasks->GetTasks()[i];
        sleep_entry_t& entry = SleepQueue[i];

        if (!tasks->RetainForDispatch(task))
        {
            continue;
        }

        const bool awakened = entry.task == task && !entry.waiting && !entry.dispatched;

        if (!task->allocated || task->state == task_t::SWITCHING || (task->dying && !awakened) || (!task->schedulerQueued && !awakened) || (task->sleepPending && !awakened) || !(task->affinityMask & (1ULL << cpuId)) || (task->placementCpu >= 0 && task->placementCpu != cpuId)
            || (task->state == task_t::RUNNING && tasks->GetRunningTask(cpuId) != task) || task->process == nullptr || (task->process->stopped && !task->process->exited))
        {
            tasks->ReleaseDispatchClaim(task);
            continue;
        }

        if (!PolicyEligible(task, cpuId))
        {
            tasks->ReleaseDispatchClaim(task);
            continue;
        }

        if (task->scheduling->readyOrder == 0)
        {
            task->scheduling->readyOrder = ++ReadySequence;
        }

        if (MoreUrgent(task, selected, cpuId))
        {
            if (selected != nullptr)
            {
                tasks->ReleaseDispatchClaim(selected);
            }

            selected = task;
            sleeper  = awakened ? &entry : nullptr;
        }
        else
        {
            tasks->ReleaseDispatchClaim(task);
        }
    }

    if (selected == nullptr)
    {
        if (current != nullptr && ResourceLayerImportCaps->processManager->GetRunningProcess(cpuId) != nullptr && !PolicyEligible(current, cpuId) && tasks->RetainForDispatch(IdleTasks[cpuId]))
        {
            decision.task = IdleTasks[cpuId];
            decision.accepted = true;
        }

        decision.nextEvent = NextSchedulingEventLocked(nullptr, cpuId, now);
        UnlockQueues();
        return decision;
    }
    // Weighted service gives every normal task progress; inherited urgency
    // takes precedence while an owner is blocking a higher-priority waiter.
    selected->scheduleRuntime += 1ULL << ((selected->baseNice + 20) / 4);
    selected->schedulerQueued = false;

    if (sleeper != nullptr)
    {
        sleeper->dispatched = true;
    }

    decision.task = selected;
    decision.process = sleeper != nullptr ? sleeper->process : selected == current ? ResourceLayerImportCaps->processManager->GetRunningProcess(cpuId) : selected->process;
    decision.nextEvent = NextSchedulingEventLocked(selected, cpuId, now);
    decision.accepted = true;
    decision.local = true;
    UnlockQueues();
    return decision;
}

bool Scheduler::RunNextReadyProcess(uint8_t cpuId, scheduling_reason_t reason)
{
    if (cpuId >= platform.cpu_count || cpuId != arch_cpu_id())
    {
        return false;
    }

    TaskManager* tasks = ResourceLayerImportCaps->taskManager;
    task_t* current = tasks->GetCurrentTask();

    if (current != nullptr && current->schedulingBusy)
    {
        return false;
    }

    cpu_preemption_disable();

    if (current != nullptr)
    {
        current->schedulingBusy = true;
    }

    bool executed = false;

    if (current != nullptr && (!(current->affinityMask & (1ULL << cpuId)) || (current->placementCpu >= 0 && current->placementCpu != cpuId)))
    {
        ResourceLayerImportCaps->processManager->SetRunningProcess(cpuId, nullptr);
        platform.cpus[cpuId].address_space = IdleAddressSpace;
        vmm_switch_addr_space(IdleAddressSpace);
        current->schedulerQueued = true;
        executed = tasks->ExecuteTask(IdleTasks[cpuId]);
    }
    else
    {
        const auto decision = RunSchedulingAlgorithm(cpuId, reason);
        uint64_t now = 0;

        if (arch_time_read_monotonic(&now))
        {
            const uint64_t delay = decision.nextEvent > now ? decision.nextEvent - now : 1000;
            arch_program_scheduler_timer(delay);
        }

        if (decision.task == IdleTasks[cpuId])
        {
            ResourceLayerImportCaps->processManager->SetRunningProcess(cpuId, nullptr);
            platform.cpus[cpuId].address_space = IdleAddressSpace;
            vmm_switch_addr_space(IdleAddressSpace);
            executed = tasks->ExecuteTask(decision.task, true);
        }
        else if (decision.task != nullptr)
        {
            executed = ScheduleTask(decision.process, decision.task, true);

            if (!executed)
            {
                LockQueues();

                if (decision.task->allocated && !decision.task->dying && decision.task->process == decision.process)
                {
                    decision.task->schedulerQueued = true;
                }

                sleep_entry_t& sleeper = SleepQueue[decision.task - tasks->GetTasks()];

                if (sleeper.task == decision.task)
                {
                    sleeper.dispatched = false;
                }

                UnlockQueues();
            }
        }
    }

    if (current != nullptr)
    {
        current->schedulingBusy = false;
    }

    cpu_preemption_enable();
    return executed;
}

void Scheduler::RequestReschedule(uint64_t cpus)
{
    for (uint8_t cpu = 0; cpu < platform.cpu_count; ++cpu)
    {
        if (!(cpus & (1ULL << cpu)))
        {
            continue;
        }
        __atomic_store_n(&platform.cpus[cpu].reschedule_pending, true, __ATOMIC_RELEASE);
        if (cpu != arch_cpu_id() && platform.cpus[cpu].initialized)
        {
            arch_request_reschedule(cpu);
        }
    }
}

void Scheduler::ServicePendingReschedule(bool syscallBoundary)
{
    const uint8_t cpu = arch_cpu_id();

    if (!IsSchedulingActive() || platform.cpus[cpu].switching_context || platform.cpus[cpu].preemption_depth != 0)
    {
        return;
    }

    task_t* current = ResourceLayerImportCaps->taskManager->GetCurrentTask();

    if (current == nullptr || current->schedulingBusy)
    {
        return;
    }

    if (!syscallBoundary && arch_current_syscall_active() != 0)
    {
        return;
    }

    if (__atomic_exchange_n(&platform.cpus[cpu].reschedule_pending, false, __ATOMIC_ACQ_REL))
    {
        current->schedulingBusy = true;
        process_t* process = ResourceLayerImportCaps->processManager->GetCurrentProcess();

        if (process != nullptr)
        {
            EnqueueProcess(cpu, process->id);
        }

        current->schedulingBusy = false;
        RunNextReadyProcess(cpu, scheduling_reason_t::RESCHEDULE);
    }
}

bool Scheduler::KillProcess(uint64_t processId, int32_t exitStatus, bool onlyIfExited, bool* retired)
{
    if (retired != nullptr)
    {
        *retired = false;
    }

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

    // Mark before unlocking so new dispatch claims cannot start. Cleanup may
    // touch user memory and wake futexes; no scheduler lock may span it.

    for (task_t* task = process->tasks; task != nullptr; task = task->next)
    {
        taskManager->MarkDying(task);
    }

    UnlockQueues();
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
    const bool tasksRetired = process->tasks == nullptr;
    UnlockQueues();
    // No scheduler lock spans unmapping or synchronous shootdown. CPUs in a
    // timer handler can finish their short queue transaction and accept IPIs.

    for (size_t i = 0; i < retiredCount; ++i)
    {
        TaskManager::ReleaseTaskStack(retiredStacks[i]);
    }

    if (tasksRetired)
    {
        ProcessManager::ReleaseProcessResources(process);
        // Publish waitability and notify the parent only after deferred task
        // cleanup has released every child-owned file reference.
        NotifyProcessExit(process);

        process_t* parent = process->hasParent ? processManager->AcquireProcess(process->parentId) : nullptr;

        if (parent != nullptr)
        {
            __atomic_add_fetch(&parent->childSequence, 1ULL, __ATOMIC_RELEASE);
            WakeEventWaiters(&parent->childSequence);
            processManager->ReleaseProcess(parent);
        }
    }

    LockQueues();
    const bool reap = tasksRetired && (!process->waitable || process->waitConsumed);
    process->retirementActive = false;
    UnlockQueues();
    processManager->ReleaseProcess(process);

    if (reap)
    {
        (void) processManager->TryReapExitedProcess(processId);
    }

    if (retired != nullptr)
    {
        *retired = tasksRetired;
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

        if (process->exitSignal >= 32)
        {
            SignalManager::ReleaseExit(ResourceLayerImportCaps, process, true);
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
        SignalManager::ReleaseExit(ResourceLayerImportCaps, process, false);
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

uint64_t Scheduler::AcquireTask(int64_t tid, bool modify, task_t** target)
{
    ResourceLayerCaps* resources = ResourceLayerImportCaps;

    if (resources == nullptr || resources->taskManager == nullptr || resources->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    task_t* task = tid == 0 ? resources->taskManager->GetCurrentTask() : tid > 0 ? resources->taskManager->FindTask(tid) : nullptr;

    if (!resources->taskManager->RetainForDispatch(task))
    {
        return LINUX_ESRCH;
    }

    process_t* source = resources->processManager->GetCurrentProcess();
    process_t* owner  = task->process;
    uint64_t   result = 0;

    if (source == nullptr || owner == nullptr)
    {
        result = LINUX_ESRCH;
    }
    else if (modify)
    {
        spinlock_acquire_preempt(&source->credentialLock);
        const auto caller = source->credentials.user;
        spinlock_release_preempt(&source->credentialLock);
        spinlock_acquire_preempt(&owner->credentialLock);
        const auto receiver = owner->credentials.user;
        spinlock_release_preempt(&owner->credentialLock);

        if (caller.effective != 0 && caller.effective != receiver.real && caller.effective != receiver.effective)
        {
            result = static_cast<uint64_t>(-1);
        }
    }

    if (result != 0)
    {
        resources->taskManager->ReleaseDispatchClaim(task);
    }
    else
    {
        *target = task;
    }

    return result;
}

void Scheduler::SetPriorityManager(SynchronizationManager* manager)
{
    PriorityManager = manager;
}

uint64_t Scheduler::SetUserNice(task_t* task, int32_t nice)
{
    process_t* process = ResourceLayerImportCaps->processManager->GetCurrentProcess();
    if (process == nullptr || task == nullptr)
    {
        return LINUX_ESRCH;
    }
    spinlock_acquire_preempt(&process->credentialLock);
    const bool privileged = process->credentials.user.effective == 0;
    spinlock_release_preempt(&process->credentialLock);
    if (!privileged && nice < __atomic_load_n(&task->baseNice, __ATOMIC_ACQUIRE))
    {
        return static_cast<uint64_t>(-13);
    }
    return SetNice(task, nice);
}

uint64_t Scheduler::GetCpu(uint32_t* cpu, uint32_t* node) const
{
    *cpu = arch_cpu_id();
    if (node != nullptr)
    {
        const numa_node_t* target = platform.cpus[*cpu].numa_node;
        for (size_t index = 0; index < platform.numa_node_count; ++index)
        {
            if (target == &platform.numa_nodes[index])
            {
                *node = index;
                return 0;
            }
        }
        return LINUX_EOPNOTSUPP;
    }
    return 0;
}
