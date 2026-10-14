#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <klib/klib.h>
#include <platform/memory/vmm.h>
#include <platform.h>
}

static TaskManager* contextTaskManager;

extern "C" void task_context_switch_complete(void)
{
    contextTaskManager->CompleteSwitch();
}

static uint64_t task_save_and_disable_interrupts()
{
    uint64_t flags = 0;
#if defined(__x86_64__)
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags) : : "memory");
#endif
    return flags;
}

static void task_restore_interrupts(uint64_t flags)
{
#if defined(__x86_64__)
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
#elif defined(__aarch64__)
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
#endif
}
static void user_task_bootstrap_entry(void* arg)
{
    task_t::user_launch_context_t* launchContext = static_cast<task_t::user_launch_context_t*>(arg);

    if (launchContext == nullptr)
    {
        panic();
    }

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
    memset(&task->userLaunchContext, 0, sizeof(task->userLaunchContext));
    task->allocated       = allocated;
    task->state           = task_t::READY;
    task->ownerCpu        = -1;
    task->dispatchUsers   = 0;
    task->dying           = false;
    task->isUserTask      = false;
    task->sleepPending    = false;
    task->sleepBlocked    = false;
    task->syscallActive   = 0;
    task->preemptionDepth = 0;
    task->id              = id;
    memset(&task->taskContext, 0, sizeof(task->taskContext));
    task->stack = nullptr;
    task->next  = nullptr;
    task->prev  = nullptr;
}

TaskManager::TaskManager()
{
    ManagerLock = 0;
    contextTaskManager = this;

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        RunningTasks[i] = nullptr;
        SwitchingTasks[i] = nullptr;
    }

    for (size_t i = 0; i < MAX_TASKS; i++)
    {
        ResetTaskSlot(&Tasks[i], i, false);
    }
}

void TaskManager::LockManager() const
{
    cpu_preemption_disable();
    spinlock_acquire((spinlock_t*) &ManagerLock);
}

void TaskManager::UnlockManager() const
{
    spinlock_release((spinlock_t*) &ManagerLock);
    cpu_preemption_enable();
}

task_t* TaskManager::AllocateTaskUnlocked()
{
    for (size_t i = 0; i < MAX_TASKS; i++)
    {
        if (!Tasks[i].allocated)
        {
            ResetTaskSlot(&Tasks[i], i, true);
            return &Tasks[i];
        }
    }

    return nullptr;
}

task_t* TaskManager::AllocateTask()
{
    LockManager();
    task_t* task = AllocateTaskUnlocked();
    UnlockManager();
    return task;
}

task_t* TaskManager::CreateTask(arch_task_entry_t entry, void* arg, const task_t::user_launch_context_t* userLaunch)
{
    void* stackRegion = vmalloc(CPU_KERNEL_STACK_SIZE + PAGE_SIZE);
    if (stackRegion == nullptr)
        return nullptr;
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
    if (entry == nullptr)
        return nullptr;
    return CreateTask(entry, arg, nullptr);
}

task_t* TaskManager::CreateUserBootstrapTask(uint64_t userRip, uint64_t userRsp, uint64_t arg0, uint64_t arg1)
{
    const task_t::user_launch_context_t launch = {userRip, userRsp, arg0, arg1};
    return CreateTask(user_task_bootstrap_entry, nullptr, &launch);
}

bool TaskManager::FreeTask(task_t* task, void** retiredStack)
{
    void* stackToFree = nullptr;

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

    if (task->stack != nullptr)
    {
        stackToFree = task->stack;
        task->stack = nullptr;
    }

    ResetTaskSlot(task, (uint64_t) (task - &Tasks[0]), false);

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        if (RunningTasks[i] == task)
        {
            RunningTasks[i] = nullptr;
        }
    }

    UnlockManager();

    if (retiredStack != nullptr)
        *retiredStack = stackToFree;
    else
        ReleaseTaskStack(stackToFree);

    return true;
}

void TaskManager::ReleaseTaskStack(void* stack)
{
    if (stack != nullptr)
        vfree((uint8_t*) stack - PAGE_SIZE);
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

    LockManager();
    if (releaseDispatchClaim && task->dispatchUsers != 0)
        --task->dispatchUsers;
    if (!task->allocated || (task->dying && !task->sleepPending) || task->state == task_t::SWITCHING
        || (task->ownerCpu >= 0 && task->ownerCpu != cpuId) || __atomic_load_n(&task->sleepBlocked, __ATOMIC_ACQUIRE))
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

    const uint64_t interruptState = task_save_and_disable_interrupts();
    current->state = task_t::SWITCHING;
    SwitchingTasks[cpuId] = current;
    task->state = task_t::RUNNING;
    task->ownerCpu = cpuId;
    __atomic_store_n(&RunningTasks[cpuId], task, __ATOMIC_RELEASE);
    UnlockManager();
    cpu_info_t*    cpu_info       = &platform.cpus[cpuId];
    current->preemptionDepth      = cpu_info->preemption_depth;
    cpu_info->preemption_depth    = task->preemptionDepth;
#if defined(__x86_64__)
    current->syscallActive                 = cpu_info->arch_info.syscall_ctx.active;
    cpu_info->arch_info.syscall_ctx.active = task->syscallActive;
#endif
    if (task->isUserTask)
    {
        uint64_t next_stack_top = resolve_task_kernel_stack_top(task, cpu_info);
        if (next_stack_top != 0)
        {
            arch_set_user_transition_stack(next_stack_top);
        }
    }

    arch_save_switch_and_execute_context(&current->taskContext, &task->taskContext);

    // We only reach here after another switch restores this task.
    cpu_info->preemption_depth = current->preemptionDepth;
#if defined(__x86_64__)
    cpu_info->arch_info.syscall_ctx.active = current->syscallActive;
#endif
    if (current->isUserTask)
    {
        uint64_t current_stack_top = resolve_task_kernel_stack_top(current, cpu_info);
        if (current_stack_top != 0)
        {
            arch_set_user_transition_stack(current_stack_top);
        }
    }

    task_restore_interrupts(interruptState);
    return true;
}

void TaskManager::CompleteSwitch()
{
    // Called on the incoming stack after the outgoing context is fully saved,
    // before interrupts are restored. Only now is the old stack quiescent.
    LockManager();
    const uint8_t cpuId = arch_cpu_id();
    task_t* outgoing = SwitchingTasks[cpuId];
    if (outgoing != nullptr)
        outgoing->state = outgoing->dying ? task_t::DYING : task_t::SUSPENDED;
    SwitchingTasks[cpuId] = nullptr;
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
    const bool valid = task != nullptr && task >= Tasks && task < Tasks + MAX_TASKS && task->allocated
        && (!task->dying || task->sleepPending);
    if (valid)
        ++task->dispatchUsers;
    UnlockManager();
    return valid;
}

void TaskManager::ReleaseDispatchClaim(task_t* task)
{
    LockManager();
    if (task->dispatchUsers != 0)
        --task->dispatchUsers;
    UnlockManager();
}

void TaskManager::MarkDying(task_t* task)
{
    LockManager();
    if (task != nullptr && task->allocated)
    {
        task->dying = true;
        if (task->state != task_t::RUNNING && task->state != task_t::SWITCHING)
            task->state = task_t::DYING;
    }
    UnlockManager();
}

void TaskManager::FinishSleeping(task_t* task)
{
    LockManager();
    __atomic_store_n(&task->sleepBlocked, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task->sleepPending, false, __ATOMIC_RELEASE);
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
        previous->state = task_t::SUSPENDED;
    if (task != nullptr)
    {
        task->state = task_t::RUNNING;
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
