#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <klib/klib.h>
}

TaskManager::TaskManager()
{
    for (size_t i = 0; i < MAX_TASKS; i++)
    {
        Tasks[i].allocated = false;
        Tasks[i].id        = (uint64_t) i;
        memset(&Tasks[i].taskContext, 0, sizeof(Tasks[i].taskContext));
        Tasks[i].stack = nullptr;
    }
}

task_t* TaskManager::AllocateTask()
{
    for (size_t i = 0; i < MAX_TASKS; i++)
    {
        if (!Tasks[i].allocated)
        {
            Tasks[i].allocated = true;
            Tasks[i].id        = (uint64_t) i;
            memset(&Tasks[i].taskContext, 0, sizeof(Tasks[i].taskContext));
            Tasks[i].stack = nullptr;
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
        return nullptr;
    }

    void* stack_top = (void*) ((uint8_t*) task->stack + CPU_KERNEL_STACK_SIZE);
    arch_init_context(&task->taskContext, stack_top, entry, arg);

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

    memset(&task->taskContext, 0, sizeof(task->taskContext));
    task->id        = (uint64_t) (task - &Tasks[0]);
    task->allocated = false;

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