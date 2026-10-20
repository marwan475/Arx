#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/ProcessManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <klib/klib.h>
#include <platform.h>
#include <platform/memory/vmm.h>
}

extern "C" void dispatcher_account_scheduling(bool beginExecution);

static TaskManager* contextTaskManager;

size_t TaskManager::CountTasks() const
{
    LockManager();
    size_t count = 0;

    for (size_t i = 0; i < MAX_TASKS; ++i)
    {
        if (Tasks[i].allocated)
        {
            ++count;
        }
    }

    UnlockManager();
    return count;
}

bool TaskManager::GetCurrentName(char* name)
{
    LockManager();
    task_t* task = GetCurrentTask();

    if (task != nullptr)
    {
        memcpy(name, task->name, sizeof(task->name));
    }

    UnlockManager();
    return task != nullptr;
}

bool TaskManager::SetCurrentName(const char* name)
{
    LockManager();
    task_t* task = GetCurrentTask();

    if (task != nullptr)
    {
        memcpy(task->name, name, sizeof(task->name));
        task->name[15] = '\0';
    }

    UnlockManager();
    return task != nullptr;
}

bool TaskManager::GetCurrentTlsBase(bool gs, uint64_t* value)
{
    if (!arch_user_tls_supported())
    {
        return false;
    }
    cpu_preemption_disable();
    bool found = GetCurrentTask() != nullptr;

    if (found)
    {
        *value = arch_read_user_tls_base(gs);
    }

    cpu_preemption_enable();
    return found;
}

bool TaskManager::SetCurrentTlsBase(bool gs, uint64_t value)
{
    if (!arch_user_tls_supported())
    {
        return false;
    }
    cpu_preemption_disable();
    task_t* task = GetCurrentTask();

    if (task != nullptr)
    {
        arch_context_set_tls(&task->taskContext, gs, value);
        arch_write_user_tls_base(gs, value);
    }

    cpu_preemption_enable();
    return task != nullptr;
}

extern "C" void task_context_switch_complete(void)
{
    contextTaskManager->CompleteSwitch();
    dispatcher_account_scheduling(true);
}

static void user_task_bootstrap_entry(void* arg)
{
    task_t::user_launch_context_t* launchContext = static_cast<task_t::user_launch_context_t*>(arg);

    if (launchContext == nullptr)
    {
        panic();
    }

    contextTaskManager->AccountCurrent(true);
    arch_enter_user_mode(launchContext->userRip, launchContext->userRsp, launchContext->arg0, launchContext->arg1);
}

static uint64_t resolve_task_kernel_stack_top(const task_t* task, const cpu_info_t* cpu_info)
{
    if (task != nullptr)
    {
        if (task->stack != nullptr)
        {
            return (uint64_t) (uintptr_t) ((const uint8_t*) task->stack + CPU_KERNEL_STACK_SIZE);
        }

        const uint64_t context_stack_top = arch_task_context_stack_pointer(&task->taskContext);

        if (context_stack_top != 0)
        {
            return context_stack_top;
        }
    }

    if (cpu_info != nullptr && cpu_info->kernel_stack_base != nullptr && cpu_info->kernel_stack_size != 0)
    {
        return (uint64_t) (uintptr_t) ((const uint8_t*) cpu_info->kernel_stack_base + cpu_info->kernel_stack_size);
    }

    return 0;
}

void TaskManager::ResetTaskSlot(task_t* task, uint64_t id, bool allocated)
{
    ARX_DEBUG_RUN(
        const uint64_t previous = task->debug.generation;
        const bool exhausted = task->debug.exhausted || (allocated && previous == UINT64_MAX);
        task->debug = {};
        task->debug.generation = allocated && !exhausted ? previous + 1 : previous;
        task->debug.exhausted = exhausted;
    );
    memset(&task->userLaunchContext, 0, sizeof(task->userLaunchContext));
    task->allocated            = allocated;
    task->state                = task_t::READY;
    task->ownerCpu             = -1;
    task->dispatchUsers        = 0;
    task->affinityMask         = platform.cpu_count >= 64 ? UINT64_MAX : (1ULL << platform.cpu_count) - 1;
    task->scheduleRuntime      = 0;
    task->scheduling = allocated ? static_cast<task_scheduling_t*>(kzalloc(sizeof(task_scheduling_t))) : nullptr;
    if (allocated && task->scheduling == nullptr)
    {
        task->allocated = false;
    }
    if (task->scheduling != nullptr)
    {
        task->scheduling->reservationCpu = -1;
        task->scheduling->roundRobinRemaining = SCHEDULING_RR_QUANTUM;
    }
    task->baseNice             = 0;
    task->effectiveNice        = 0;
    task->placementCpu         = -1;
    task->schedulerQueued      = false;
    task->migrationEnabled     = false;
    task->schedulingBusy       = false;
    task->dying                = false;
    task->priorityCleanupDone  = false;
    task->isUserTask           = false;
    task->sleepPending         = false;
    task->sleepBlocked         = false;
    task->sleepUninterruptible = false;
    task->syscallActive        = 0;
    task->accountingStamp      = 0;
    task->accountingRunning    = false;
    task->accountingUser       = false;
    task->preemptionDepth      = 0;
    task->id                   = id;
    task->process              = nullptr;
    task->signalMask           = 0;
    task->signalPending        = 0;
    task->signalSequence       = 0;
    task->signalSavedMask      = 0;
    task->signalRestoreMask    = false;
    task->signalStack          = {0, 2, 0, 0};
    task->signalDepth          = 0;
    task->signalSenders        = nullptr;
    task->signalQueue          = nullptr;
    task->robustHead           = 0;
    task->clearChildTid        = 0;
    task->setChildTid          = 0;
    task->robustOwner          = nullptr;
    task->descriptorTable      = nullptr;
    task->filesystemContext    = nullptr;
    task->retirementCleanup    = nullptr;
    memset(task->name, 0, sizeof(task->name));
    memcpy(task->name, "Arx", 3);
    memset(&task->taskContext, 0, sizeof(task->taskContext));
    task->stack                = nullptr;
    task->next                 = nullptr;
    task->prev                 = nullptr;
}

TaskManager::TaskManager()
{
    static_assert(sizeof(TaskManager) <= 131072, "TaskManager must fit the largest heap object");
    ARX_DEBUG_RUN(
        DebugStats.source = this;
        DebugStats.records = Tasks;
        DebugStats.running = RunningTasks;
        DebugStats.switching = SwitchingTasks;
        DebugStats.capacity = MAX_TASKS;
    );
    PriorityCleanup = nullptr;
    ManagerLock     = 0;
    LoadEpoch       = 0;

    memset(LoadAverages, 0, sizeof(LoadAverages));
    contextTaskManager = this;

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        RunningTasks[i]   = nullptr;
        SwitchingTasks[i] = nullptr;
    }

    for (size_t i = 0; i < MAX_TASKS; i++)
    {
        ARX_DEBUG_RUN(Tasks[i].debug = {});
        ResetTaskSlot(&Tasks[i], i, false);
    }
}

void TaskManager::LockManager() const
{
    cpu_preemption_disable();
    spinlock_acquire((spinlock_t*) &ManagerLock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&const_cast<TaskManager*>(this)->DebugStats.group));
}

void TaskManager::UnlockManager() const
{
    ARX_DEBUG_RUN(arx_debug_group_end(&const_cast<TaskManager*>(this)->DebugStats.group));
    spinlock_release((spinlock_t*) &ManagerLock);
    cpu_preemption_enable();
}

task_t* TaskManager::AllocateTaskUnlocked()
{
    for (size_t i = 0; i < MAX_TASKS; i++)
    {
        if (!Tasks[i].allocated && FindTask(i) == nullptr)
        {
            ResetTaskSlot(&Tasks[i], i, true);
            ARX_DEBUG_RUN(DebugAllocated(&Tasks[i]));
            return Tasks[i].allocated ? &Tasks[i] : nullptr;
        }
    }

    return nullptr;
}

task_t* TaskManager::AllocateTask()
{
    ARX_DEBUG_ONLY(DebugOperation debugOperation(DebugStats.allocations);)

    LockManager();
    task_t* task = AllocateTaskUnlocked();
    UnlockManager();
    ARX_DEBUG_RUN(
        if (task != nullptr)
        {
            debugOperation.Finish();
        }
    );
    return task;
}

task_t* TaskManager::CreateTask(arch_task_entry_t entry, void* arg, const task_t::user_launch_context_t* userLaunch)
{
    void* stackRegion = vmalloc(CPU_KERNEL_STACK_SIZE + PAGE_SIZE);

    if (stackRegion == nullptr)
    {
        return nullptr;
    }

    vmm_unmap_page((uintptr_t) stackRegion, vmm_kernel_space());
    void* allocatedStack = (uint8_t*) stackRegion + PAGE_SIZE;
    LockManager();
    task_t* task = AllocateTaskUnlocked();

    if (task == nullptr)
    {
        UnlockManager();
        vfree(stackRegion);
        return nullptr;
    }

    task->stack      = allocatedStack;
    task->isUserTask = userLaunch != nullptr;

    if (userLaunch != nullptr)
    {
        task->userLaunchContext = *userLaunch;
        arg                     = &task->userLaunchContext;
    }

    void* stack_top = (void*) ((uint8_t*) task->stack + CPU_KERNEL_STACK_SIZE);
    arch_init_context(&task->taskContext, stack_top, entry, arg);
    UnlockManager();
    return task;
}

task_t* TaskManager::CreateKernelTask(arch_task_entry_t entry, void* arg)
{
    ARX_DEBUG_ONLY(DebugOperation debugOperation(DebugStats.creations);)

    if (entry == nullptr)
    {
        return nullptr;
    }

    task_t* task = CreateTask(entry, arg, nullptr);
    ARX_DEBUG_RUN(
        if (task != nullptr)
        {
            debugOperation.Finish();
        }
    );
    return task;
}

task_t* TaskManager::CreateUserBootstrapTask(uint64_t userRip, uint64_t userRsp, uint64_t arg0, uint64_t arg1)
{
    ARX_DEBUG_ONLY(DebugOperation debugOperation(DebugStats.creations);)

    const task_t::user_launch_context_t launch = {userRip, userRsp, arg0, arg1};
    task_t* task = CreateTask(user_task_bootstrap_entry, nullptr, &launch);
    ARX_DEBUG_RUN(
        if (task != nullptr)
        {
            debugOperation.Finish();
        }
    );
    return task;
}

static void cloned_user_entry(void* argument)
{
    task_t* task = static_cast<task_t*>(argument);

    if (task->setChildTid != 0)
    {
        const uint32_t tid = static_cast<uint32_t>(task->id);
        (void) safe_write_user(task->setChildTid, &tid, sizeof(tid));
        task->setChildTid = 0;
    }

    const auto* frame = arch_clone_frame(static_cast<uint8_t*>(task->stack) + CPU_KERNEL_STACK_SIZE);
    contextTaskManager->AccountCurrent(true);
    arch_resume_user_frame(frame);
}

task_t* TaskManager::CreateUserCloneTask(const arch_syscall_frame_t* frame, uint64_t id)
{
    ARX_DEBUG_ONLY(DebugOperation debugOperation(DebugStats.creations);)

    if (!arch_task_context_supported() || frame == nullptr || id >= MAX_TASKS)
    {
        return nullptr;
    }

    void* allocation = vmalloc(CPU_KERNEL_STACK_SIZE + PAGE_SIZE);

    if (allocation == nullptr)
    {
        return nullptr;
    }

    vmm_unmap_page(reinterpret_cast<uintptr_t>(allocation), vmm_kernel_space());
    LockManager();
    task_t* task = &Tasks[id];

    if (task->allocated || FindTask(id) != nullptr)
    {
        UnlockManager();
        vfree(allocation);
        return nullptr;
    }

    ResetTaskSlot(task, id, true);
    if (!task->allocated)
    {
        UnlockManager();
        vfree(allocation);
        return nullptr;
    }
    ARX_DEBUG_RUN(DebugAllocated(task));
    task->stack      = static_cast<uint8_t*>(allocation) + PAGE_SIZE;
    task->isUserTask = true;
    void* stack_top  = static_cast<uint8_t*>(task->stack) + CPU_KERNEL_STACK_SIZE;
    arch_prepare_clone_context(&task->taskContext, frame, stack_top, cloned_user_entry, task);
    UnlockManager();
    ARX_DEBUG_RUN(
        if (task != nullptr)
        {
            debugOperation.Finish();
        }
    );
    return task;
}

bool TaskManager::FreeTask(task_t* task, void** retiredStack, retired_task_resources_t* retiredResources)
{
    ARX_DEBUG_ONLY(DebugOperation debugOperation(DebugStats.reclamations);)

    void*            stackToFree   = nullptr;
    signal_sender_t* sendersToFree = nullptr;

    LockManager();

    if (task == nullptr)
    {
        UnlockManager();
        return false;
    }

    if (task < &Tasks[0] || task >= &Tasks[MAX_TASKS])
    {
        UnlockManager();
        return false;
    }

    if (!task->allocated || task->dispatchUsers != 0 || task->state == task_t::RUNNING || task->state == task_t::SWITCHING || __atomic_load_n(&task->sleepPending, __ATOMIC_ACQUIRE))
    {
        UnlockManager();
        return false;
    }

    if (task->retirementCleanup != nullptr || (PriorityCleanup != nullptr && !task->priorityCleanupDone))
    {
        const bool priorityCleanup = PriorityCleanup != nullptr && !task->priorityCleanupDone;
        task->priorityCleanupDone = true;
        auto  cleanup           = task->retirementCleanup;
        task->retirementCleanup = nullptr;
        task->dying             = true;
        task->state             = task_t::DYING;
        ++task->dispatchUsers;
        UnlockManager();

        if (priorityCleanup)
        {
            dispatcher_cleanup_task(PriorityCleanup, task);
        }

        if (cleanup != nullptr)
        {
            dispatcher_cleanup_task(cleanup, task);
        }

        LockManager();
        --task->dispatchUsers;
    }

    if (task->stack != nullptr)
    {
        stackToFree = task->stack;
        task->stack = nullptr;
    }

    retired_task_resources_t resources = {task->descriptorTable, task->filesystemContext, nullptr, nullptr};
    process_t*               process   = task->process;

    if (process != nullptr)
    {
        task_t* survivor       = nullptr;
        bool    tableUsed      = false;
        bool    filesystemUsed = false;

        for (task_t* sibling = process->tasks; sibling != nullptr; sibling = sibling->next)
        {
            if (sibling != task && sibling->allocated)
            {
                survivor       = sibling;
                tableUsed      = tableUsed || sibling->descriptorTable == process->descriptorTable;
                filesystemUsed = filesystemUsed || sibling->filesystemContext == process->filesystemContext;
            }
        }

        // The process defaults are fallback references, not extra users that
        // keep a departed thread's private descriptors open indefinitely.
        if (!tableUsed && process->descriptorTable == resources.table)
        {
            resources.defaultTable   = process->descriptorTable;
            process->descriptorTable = survivor != nullptr ? survivor->descriptorTable : nullptr;

            if (process->descriptorTable != nullptr)
            {
                __atomic_add_fetch(&process->descriptorTable->references, 1U, __ATOMIC_RELAXED);
            }
        }

        if (!filesystemUsed && process->filesystemContext == resources.filesystem)
        {
            resources.defaultFilesystem = process->filesystemContext;
            process->filesystemContext  = survivor != nullptr ? survivor->filesystemContext : nullptr;

            if (process->filesystemContext != nullptr)
            {
                __atomic_add_fetch(&process->filesystemContext->references, 1U, __ATOMIC_RELAXED);
            }
        }
    }

    task_scheduling_t* schedulingToFree = task->scheduling;
    signal_queue_t* queueToFree = task->signalQueue;
    sendersToFree = task->signalSenders;
    ARX_DEBUG_ADD(DebugStats.reclaimed, 1);
    ARX_DEBUG_ADD(DebugStats.liveSlots, UINT64_MAX);
    ResetTaskSlot(task, (uint64_t) (task - &Tasks[0]), false);

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        if (RunningTasks[i] == task)
        {
            RunningTasks[i] = nullptr;
        }
    }

    UnlockManager();
    if (retiredResources != nullptr)
    {
        *retiredResources = resources;
    }
    else
    {
        ReleaseTaskResources(resources);
    }
    kfree(sendersToFree);
    kfree(queueToFree);
    kfree(schedulingToFree);

    if (retiredStack != nullptr)
    {
        *retiredStack = stackToFree;
    }
    else
    {
        ReleaseTaskStack(stackToFree);
    }

    ARX_DEBUG_RUN(debugOperation.Finish());
    return true;
}

void TaskManager::ReleaseTaskResources(const retired_task_resources_t& resources)
{
    ProcessManager::ReleaseDescriptorTable(resources.table);
    ProcessManager::ReleaseFilesystemContext(resources.filesystem);
    ProcessManager::ReleaseDescriptorTable(resources.defaultTable);
    ProcessManager::ReleaseFilesystemContext(resources.defaultFilesystem);
}

void TaskManager::ReleaseTaskStack(void* stack)
{
    if (stack != nullptr)
    {
        vfree((uint8_t*) stack - PAGE_SIZE);
    }
}

bool TaskManager::ExecuteTask(task_t* task, bool releaseDispatchClaim)
{
    if (task == nullptr)
    {
        return false;
    }

    if (task < &Tasks[0] || task >= &Tasks[MAX_TASKS])
    {
        return false;
    }

    uint8_t cpuId = arch_cpu_id();

    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    dispatcher_account_scheduling(false);
    LockManager();

    if (releaseDispatchClaim && task->dispatchUsers != 0)
    {
        --task->dispatchUsers;
    }

    if (!task->allocated || (task->dying && !task->sleepPending) || task->state == task_t::SWITCHING || (task->ownerCpu >= 0 && task->ownerCpu != cpuId && !__atomic_load_n(&task->migrationEnabled, __ATOMIC_ACQUIRE)) || (task->state == task_t::RUNNING && RunningTasks[cpuId] != task)
        || !(__atomic_load_n(&task->affinityMask, __ATOMIC_ACQUIRE) & (1ULL << cpuId)) || __atomic_load_n(&task->sleepBlocked, __ATOMIC_ACQUIRE))
    {
        UnlockManager();
        return false;
    }

    task_t* current = RunningTasks[cpuId];

    if (current == nullptr)
    {
        UnlockManager();
        return false;
    }

    if (current == task)
    {
        UnlockManager();
        return true;
    }

    const uint64_t interruptState = arch_save_and_disable_interrupts();
    AccountCurrent(current->accountingUser);
    current->accountingRunning    = false;
    task->accountingRunning       = arch_time_read_monotonic(&task->accountingStamp);
    platform.cpus[cpuId].switching_context = true;
    current->state                = task_t::SWITCHING;
    SwitchingTasks[cpuId]         = current;
    task->state                   = task_t::RUNNING;
    task->ownerCpu                = cpuId;
    __atomic_store_n(&RunningTasks[cpuId], task, __ATOMIC_RELEASE);
    UnlockManager();
    cpu_info_t* cpu_info       = &platform.cpus[cpuId];
    current->preemptionDepth   = cpu_info->preemption_depth;
    cpu_info->preemption_depth = task->preemptionDepth;
    current->syscallActive     = arch_current_syscall_active();
    arch_set_current_syscall_active(task->syscallActive);

    if (task->isUserTask)
    {
        uint64_t next_stack_top = resolve_task_kernel_stack_top(task, cpu_info);

        if (next_stack_top != 0)
        {
            arch_set_user_transition_stack(next_stack_top);
        }
    }

    arch_save_switch_and_execute_context(&current->taskContext, &task->taskContext);

    // A suspended task may resume on a different CPU. Never reuse its old
    // per-CPU pointer after restoring the task context.
    cpu_info                   = &platform.cpus[arch_cpu_id()];
    cpu_info->preemption_depth = current->preemptionDepth;
    arch_set_current_syscall_active(current->syscallActive);

    if (current->isUserTask)
    {
        uint64_t current_stack_top = resolve_task_kernel_stack_top(current, cpu_info);

        if (current_stack_top != 0)
        {
            arch_set_user_transition_stack(current_stack_top);
        }
    }

    arch_restore_interrupts(interruptState);
    return true;
}

void TaskManager::CompleteSwitch()
{
    // Called on the incoming stack after the outgoing context is fully saved,
    // before interrupts are restored. Only now is the old stack quiescent.
    LockManager();
    const uint8_t cpuId    = arch_cpu_id();
    task_t*       outgoing = SwitchingTasks[cpuId];

    if (outgoing != nullptr)
    {
        outgoing->state = outgoing->dying ? task_t::DYING : task_t::SUSPENDED;
    }

    SwitchingTasks[cpuId] = nullptr;
    platform.cpus[cpuId].switching_context = false;
    UnlockManager();
}

bool TaskManager::MarkSleeping(task_t* task)
{
    LockManager();
    bool result = task != nullptr && task == GetCurrentTask() && task->allocated && !task->sleepPending;

    if (result)
    {
        __atomic_store_n(&task->sleepPending, true, __ATOMIC_RELEASE);
        __atomic_store_n(&task->sleepBlocked, true, __ATOMIC_RELEASE);
    }

    UnlockManager();
    return result;
}

bool TaskManager::RetainForDispatch(task_t* task)
{
    LockManager();
    const bool valid = task != nullptr && task >= Tasks && task < Tasks + MAX_TASKS && task->allocated && (!task->dying || task->sleepPending);

    if (valid)
    {
        ++task->dispatchUsers;
    }

    UnlockManager();
    return valid;
}

void TaskManager::ReleaseDispatchClaim(task_t* task)
{
    LockManager();

    if (task->dispatchUsers != 0)
    {
        --task->dispatchUsers;
    }

    UnlockManager();
}

bool TaskManager::CleanupRetiredTask(task_t* task)
{
    LockManager();

    if (task == nullptr || task < &Tasks[0] || task >= &Tasks[MAX_TASKS] || !task->allocated || !task->dying || task->dispatchUsers != 0 || task->state == task_t::RUNNING || task->state == task_t::SWITCHING || __atomic_load_n(&task->sleepPending, __ATOMIC_ACQUIRE))
    {
        UnlockManager();
        return false;
    }

    const bool priorityCleanup = PriorityCleanup != nullptr && !task->priorityCleanupDone;
    task->priorityCleanupDone = true;
    auto  cleanup           = task->retirementCleanup;
    task->retirementCleanup = nullptr;
    ++task->dispatchUsers;
    UnlockManager();

    if (priorityCleanup)
    {
        dispatcher_cleanup_task(PriorityCleanup, task);
    }

    if (cleanup != nullptr)
    {
        dispatcher_cleanup_task(cleanup, task);
    }

    LockManager();
    --task->dispatchUsers;
    UnlockManager();
    return true;
}

void TaskManager::SetPriorityCleanup(const dispatcher_task_cleanup_t* cleanup)
{
    PriorityCleanup = cleanup;
}

void TaskManager::CleanupCurrentTask()
{
    cpu_preemption_disable();
    task_t* task = GetCurrentTask();

    if (task != nullptr && PriorityCleanup != nullptr && !task->priorityCleanupDone)
    {
        task->priorityCleanupDone = task->dying;
        dispatcher_cleanup_task(PriorityCleanup, task);
    }

    if (task != nullptr && task->retirementCleanup != nullptr)
    {
        auto cleanup            = task->retirementCleanup;
        task->retirementCleanup = nullptr;
        dispatcher_cleanup_task(cleanup, task);
    }

    cpu_preemption_enable();
}

void TaskManager::MarkDying(task_t* task)
{
    LockManager();

    if (task != nullptr && task->allocated)
    {
        task->dying = true;

        if (task->state != task_t::RUNNING && task->state != task_t::SWITCHING)
        {
            task->state = task_t::DYING;
        }
    }

    UnlockManager();
}

void TaskManager::FinishSleeping(task_t* task)
{
    LockManager();
    __atomic_store_n(&task->sleepBlocked, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task->sleepPending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task->sleepUninterruptible, false, __ATOMIC_RELEASE);
    UnlockManager();
}

task_t* TaskManager::GetRunningTask(uint8_t cpuId) const
{
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return nullptr;
    }

    return __atomic_load_n(&RunningTasks[cpuId], __ATOMIC_ACQUIRE);
}

task_t* TaskManager::GetCurrentTask() const
{
    return GetRunningTask(arch_cpu_id());
}

void TaskManager::AccountActive(bool userMode)
{
    if (contextTaskManager != nullptr)
    {
        contextTaskManager->AccountCurrent(userMode);
    }
}

void TaskManager::AccountCurrent(bool userMode)
{
    const uint64_t flags = arch_save_and_disable_interrupts();
    task_t*        task  = GetCurrentTask();
    uint64_t       now   = 0;

    if (task != nullptr && arch_time_read_monotonic(&now))
    {
        if (task->accountingRunning && now >= task->accountingStamp && task->isUserTask && task->process != nullptr)
        {
            uint64_t* counter = task->accountingUser ? &task->process->userCpuNanoseconds : &task->process->kernelCpuNanoseconds;
            __atomic_fetch_add(counter, now - task->accountingStamp, __ATOMIC_RELAXED);
        }

        task->accountingStamp   = now;
        task->accountingRunning = true;
        task->accountingUser    = userMode;
    }

    arch_restore_interrupts(flags);
}

bool TaskManager::HasRunningTasks(const process_t* process) const
{
    LockManager();
    bool running = false;

    for (size_t cpu = 0; cpu < BOOT_SMP_MAX_CPUS; ++cpu)
    {
        running |= (RunningTasks[cpu] != nullptr && RunningTasks[cpu]->process == process) ||
                   (SwitchingTasks[cpu] != nullptr && SwitchingTasks[cpu]->process == process);
    }

    UnlockManager();
    return running;
}

void TaskManager::ReadSystemStatistics(uint64_t now, uint64_t loads[3], uint16_t* liveTasks)
{
    LockManager();
    uint64_t active = 0;
    uint16_t live   = 0;

    for (size_t index = 0; index < MAX_TASKS; ++index)
    {
        const task_t& task    = Tasks[index];
        process_t*    process = __atomic_load_n(&task.process, __ATOMIC_ACQUIRE);

        if (task.allocated && !task.dying && process != nullptr && !__atomic_load_n(&process->exited, __ATOMIC_ACQUIRE))
        {
            ++live;

            if (!__atomic_load_n(&process->stopped, __ATOMIC_ACQUIRE) &&
                (!__atomic_load_n(&task.sleepBlocked, __ATOMIC_ACQUIRE) || __atomic_load_n(&task.sleepUninterruptible, __ATOMIC_ACQUIRE)))
            {
                ++active;
            }
        }
    }

    // Linux fixed-point exponential averages sampled every five seconds.
    // Exponentiation handles a delayed sample without an unbounded catch-up loop.
    if (now >= LoadEpoch && now - LoadEpoch >= 5000000000ULL)
    {
        const uint64_t samples = (now - LoadEpoch) / 5000000000ULL;
        const uint64_t decay[3] = {1884, 2014, 2037};

        for (size_t index = 0; index < 3; ++index)
        {
            uint64_t factor = 2048;
            uint64_t base   = decay[index];

            for (uint64_t count = samples; count != 0; count >>= 1)
            {
                if ((count & 1) != 0)
                {
                    factor = (factor * base + 1024) / 2048;
                }

                base = (base * base + 1024) / 2048;
            }

            LoadAverages[index] = (LoadAverages[index] * factor + active * 65536 * (2048 - factor) + 1024) / 2048;
        }

        LoadEpoch += samples * 5000000000ULL;
    }

    memcpy(loads, LoadAverages, sizeof(LoadAverages));
    *liveTasks = live;
    UnlockManager();
}

task_t* TaskManager::FindTask(uint64_t id) const
{
    for (size_t index = 0; index < MAX_TASKS; ++index)
    {
        if (Tasks[index].allocated && Tasks[index].id == id)
        {
            return const_cast<task_t*>(&Tasks[index]);
        }
    }

    return nullptr;
}

void TaskManager::SetCurrentIdentity(uint64_t id)
{
    LockManager();
    ARX_DEBUG_ADD(DebugStats.identityChanges, GetCurrentTask()->id != id ? 1 : 0);
    GetCurrentTask()->id = id;
    UnlockManager();
}

bool TaskManager::SetRunningTask(uint8_t cpuId, task_t* task)
{
    LockManager();

    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        UnlockManager();
        return false;
    }

    if (task != nullptr)
    {
        if (task < &Tasks[0] || task >= &Tasks[MAX_TASKS])
        {
            UnlockManager();
            return false;
        }

        if (!task->allocated || task->state == task_t::SWITCHING || (task->ownerCpu >= 0 && task->ownerCpu != cpuId))
        {
            UnlockManager();
            return false;
        }
    }

    task_t* previous = RunningTasks[cpuId];

    if (previous != nullptr && previous != task)
    {
        if (cpuId == arch_cpu_id())
        {
            AccountCurrent(previous->accountingUser);
            previous->accountingRunning = false;
        }

        previous->state = task_t::SUSPENDED;
    }

    if (task != nullptr)
    {
        if (task != previous)
        {
            task->accountingRunning = arch_time_read_monotonic(&task->accountingStamp);
        }

        ARX_DEBUG_RUN(task->debug.published = true);
        task->state    = task_t::RUNNING;
        task->ownerCpu = cpuId;
    }

    __atomic_store_n(&RunningTasks[cpuId], task, __ATOMIC_RELEASE);
    UnlockManager();
    return true;
}

task_t* TaskManager::GetTasks()
{
    return Tasks;
}

const task_t* TaskManager::GetTasks() const
{
    return Tasks;
}

size_t TaskManager::GetCapacity() const
{
    return MAX_TASKS;
}

task_t* TaskManager::GetActiveTask()
{
    return contextTaskManager != nullptr ? contextTaskManager->GetCurrentTask() : nullptr;
}
