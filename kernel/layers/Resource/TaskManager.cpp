#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <klib/klib.h>
#include <platform.h>
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

TaskManager::TaskManager()
{
    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        RunningTasks[i] = nullptr;
    }

    for (size_t i = 0; i < MAX_TASKS; i++)
    {
        memset(&Tasks[i].userLaunchContext, 0, sizeof(Tasks[i].userLaunchContext));
        Tasks[i].allocated = false;
        Tasks[i].isUserTask = false;
        Tasks[i].id        = (uint64_t) i;
        memset(&Tasks[i].taskContext, 0, sizeof(Tasks[i].taskContext));
        Tasks[i].stack = nullptr;
        Tasks[i].next  = nullptr;
        Tasks[i].prev  = nullptr;
    }

}

task_t* TaskManager::AllocateTask()
{
    for (size_t i = 0; i < MAX_TASKS; i++)
    {
        if (!Tasks[i].allocated)
        {
            memset(&Tasks[i].userLaunchContext, 0, sizeof(Tasks[i].userLaunchContext));
            Tasks[i].allocated = true;
            Tasks[i].isUserTask = false;
            Tasks[i].id        = (uint64_t) i;
            memset(&Tasks[i].taskContext, 0, sizeof(Tasks[i].taskContext));
            Tasks[i].stack = nullptr;
            Tasks[i].next  = nullptr;
            Tasks[i].prev  = nullptr;
            return &Tasks[i];
        }
    }

    return nullptr;
}

task_t* TaskManager::CreateKernelTask(arch_task_entry_t entry, void* arg)
{
    if (entry == nullptr)
    {
        return nullptr;
    }

    task_t* task = AllocateTask();
    if (task == nullptr)
    {
        return nullptr;
    }

    task->stack = vmalloc(CPU_KERNEL_STACK_SIZE);
    if (task->stack == nullptr)
    {
        task->allocated = false;
        task->isUserTask = false;
        return nullptr;
    }

    task->isUserTask = false;

    void* stack_top = (void*) ((uint8_t*) task->stack + CPU_KERNEL_STACK_SIZE);
    arch_init_context(&task->taskContext, stack_top, entry, arg);

    return task;
}

task_t* TaskManager::CreateUserBootstrapTask(uint64_t userRip, uint64_t userRsp, uint64_t arg0, uint64_t arg1)
{
    task_t* task = AllocateTask();
    if (task == nullptr)
    {
        return nullptr;
    }

    task->stack = vmalloc(CPU_KERNEL_STACK_SIZE);
    if (task->stack == nullptr)
    {
        task->allocated = false;
        task->isUserTask = false;
        memset(&task->userLaunchContext, 0, sizeof(task->userLaunchContext));
        return nullptr;
    }

    task->isUserTask                = true;
    task->userLaunchContext.userRip = userRip;
    task->userLaunchContext.userRsp = userRsp;
    task->userLaunchContext.arg0    = arg0;
    task->userLaunchContext.arg1    = arg1;

    void* stack_top = (void*) ((uint8_t*) task->stack + CPU_KERNEL_STACK_SIZE);
    arch_init_context(&task->taskContext, stack_top, user_task_bootstrap_entry, &task->userLaunchContext);

    return task;
}

bool TaskManager::FreeTask(task_t* task)
{
    if (task == nullptr)
    {
        return false;
    }

    if (task < &Tasks[0] || task >= &Tasks[MAX_TASKS])
    {
        return false;
    }

    if (!task->allocated)
    {
        return false;
    }

    if (task->stack != nullptr)
    {
        vfree(task->stack);
        task->stack = nullptr;
    }

    memset(&task->userLaunchContext, 0, sizeof(task->userLaunchContext));
    memset(&task->taskContext, 0, sizeof(task->taskContext));
    task->id        = (uint64_t) (task - &Tasks[0]);
    task->allocated = false;
    task->isUserTask = false;
    task->next      = nullptr;
    task->prev      = nullptr;

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        if (RunningTasks[i] == task)
        {
            RunningTasks[i] = nullptr;
        }
    }

    return true;
}

bool TaskManager::ExecuteTask(task_t* task)
{
    if (task == nullptr)
    {
        return false;
    }

    if (task < &Tasks[0] || task >= &Tasks[MAX_TASKS])
    {
        return false;
    }

    if (!task->allocated)
    {
        return false;
    }

    uint8_t cpuId = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    task_t* current = RunningTasks[cpuId];
    if (current == nullptr)
    {
        return false;
    }

    if (current == task)
    {
        return true;
    }

    cpu_info_t* cpu_info = &platform.cpus[cpuId];
    if (task->isUserTask)
    {
        uint64_t next_stack_top = resolve_task_kernel_stack_top(task, cpu_info);
        if (next_stack_top != 0)
        {
            arch_set_user_transition_stack(next_stack_top);
        }
    }

    RunningTasks[cpuId] = task;
    arch_save_switch_and_execute_context(&current->taskContext, &task->taskContext);

    // We only reach here after another switch restores this task.
    if (current->isUserTask)
    {
        uint64_t current_stack_top = resolve_task_kernel_stack_top(current, cpu_info);
        if (current_stack_top != 0)
        {
            arch_set_user_transition_stack(current_stack_top);
        }
    }

    RunningTasks[cpuId] = current;
    return true;
}

task_t* TaskManager::GetRunningTask(uint8_t cpuId) const
{
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return nullptr;
    }

    return RunningTasks[cpuId];
}

task_t* TaskManager::GetCurrentTask() const
{
    return GetRunningTask(arch_cpu_id());
}

bool TaskManager::SetRunningTask(uint8_t cpuId, task_t* task)
{
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    if (task != nullptr)
    {
        if (task < &Tasks[0] || task >= &Tasks[MAX_TASKS])
        {
            return false;
        }

        if (!task->allocated)
        {
            return false;
        }
    }

    RunningTasks[cpuId] = task;
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