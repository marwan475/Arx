#pragma once

#include <stddef.h>
#include <stdint.h>

struct task_t
{
    uint64_t id;
};

class TaskManager
{
public:
    static constexpr size_t MAX_TASKS = 64;

    TaskManager();

    task_t*       GetTasks();
    const task_t* GetTasks() const;

    size_t GetCapacity() const;

private:
    task_t Tasks[MAX_TASKS];
};