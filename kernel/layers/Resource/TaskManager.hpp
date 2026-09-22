#pragma once

#include <arch/arch.h>
#include <boot/boot.h>
#include <stddef.h>
#include <stdint.h>

struct task_t
{
    bool                     allocated;
    uint64_t                 id;
    struct arch_task_context taskContext;
    void*                    stack;
    task_t*                  next;
    task_t*                  prev;
};

class TaskManager
{
public:
    static constexpr size_t MAX_TASKS = 64;

    TaskManager();

    task_t* AllocateTask();
    task_t* CreateKernelTask(arch_task_entry_t entry, void* arg);
    bool    FreeTask(task_t* task);
    bool    ExecuteTask(task_t* task);

    task_t* GetRunningTask(uint8_t cpuId) const;
    bool    SetRunningTask(uint8_t cpuId, task_t* task);
    task_t* GetCurrentTask() const;

    task_t*       GetTasks();
    const task_t* GetTasks() const;

    size_t GetCapacity() const;

private:
    task_t  Tasks[MAX_TASKS];
    task_t* RunningTasks[BOOT_SMP_MAX_CPUS];
};