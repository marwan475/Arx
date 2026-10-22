#pragma once

#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TaskManager.hpp"
#include <selftests/PollBudget.hpp>

// FreeProcess temporarily pins a shared root with memoryUsers while reclaiming
// a peer fixture. CreateProcess may reject that root during the pin. Preserve
// the shared-root stress test, but allow bounded retries for this contention.
static inline process_t* smp_create_test_process(ProcessManager* manager, virt_addr_space_t* space)
{
    for (unsigned attempt = 0; attempt < 1024; ++attempt)
    {
        process_t* process = manager->CreateProcess(space);
        if (process != nullptr)
        {
            return process;
        }

        arch_pause();
    }
    return nullptr;
}

// The fixture must already have returned to its caller and restored the running
// process. Other CPUs can still briefly retain it during scheduler scans. Keep
// interrupts enabled so those observers and shared-root shootdowns can finish.
static inline bool smp_free_test_fixture(ProcessManager* processes, TaskManager* tasks,
                                         process_t* process, task_t* task)
{
    if (task != nullptr)
    {
        // Unlink before freeing makes the task slot available to another CPU.
        if (process != nullptr && task->process == process)
        {
            processes->RemoveTask(process, task);
        }

        PollBudget budget;

        while (!tasks->FreeTask(task))
        {
            if (budget.Expired())
            {
                // The detached task still borrows its process resources.
                return false;
            }

            arch_pause();
        }
    }

    if (process != nullptr)
    {
        PollBudget budget;

        while (!processes->FreeProcess(process))
        {
            if (budget.Expired())
            {
                return false;
            }

            arch_pause();
        }
    }

    return true;
}
