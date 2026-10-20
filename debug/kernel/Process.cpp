#include "DebugManager.hpp"
#include "stats/ProcessDebugStats.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TaskManager.hpp"

#if DEBUG

static arx_debug_view_t processViews[4];
static arx_debug_view_t taskViews[3];

void ProcessManager::DebugAllocated(process_t* process)
{
    (void) process;
    ARX_DEBUG_ADD(DebugStats.slotAllocations, 1);
    ARX_DEBUG_ADD(DebugStats.liveSlots, 1);
}

void TaskManager::DebugAllocated(task_t* task)
{
    if (task->allocated)
    {
        ARX_DEBUG_ADD(DebugStats.slotAllocations, 1);
        ARX_DEBUG_ADD(DebugStats.liveSlots, 1);
    }
}

void ProcessManager::DebugParent(process_t* process, bool changed)
{
    arx_debug_group_begin(&DebugStats.group);
    ProcessSlotDebug& record = process->debug;
    const uint64_t previousGeneration = record.parentGeneration;
    const bool preserveParent = record.parentCaptured && !changed && record.parentId == process->parentId;
    record.parentCaptured = true;
    record.parentId = process->parentId;
    record.parentTaskId = process->parentTaskId;
    record.recipientAddress = reinterpret_cast<uintptr_t>(process->exitRecipient);
    record.parentGeneration = 0;
    record.parentTaskGeneration = 0;
    record.parentTaskSlot = UINT64_MAX;
    record.recipientGeneration = 0;

    if (process->hasParent && preserveParent)
    {
        record.parentGeneration = previousGeneration;
    }
    else if (process->hasParent && process->parentId < MAX_PROCESSES)
    {
        const process_t& parent = Processes[process->parentId];

        if (parent.allocated && !parent.debug.exhausted)
        {
            record.parentGeneration = parent.debug.generation;
        }
    }

    if (DebugStats.tasks != nullptr && process->hasParent)
    {
        task_t* parentTask = DebugStats.tasks->FindTask(process->parentTaskId);

        if (parentTask != nullptr && !parentTask->debug.exhausted
            && process->parentId < MAX_PROCESSES && parentTask->process == &Processes[process->parentId])
        {
            record.parentTaskGeneration = parentTask->debug.generation;
            record.parentTaskSlot = parentTask - DebugStats.tasks->GetTasks();
        }
    }

    // exitRecipient is an owning retained reference, not just a PID lookup.
    if (process->exitRecipient != nullptr && !process->exitRecipient->debug.exhausted)
    {
        record.recipientGeneration = process->exitRecipient->debug.generation;
    }

    arx_debug_group_end(&DebugStats.group);
}

void ProcessManager::DebugPublished(process_t* process)
{
    arx_debug_group_begin(&DebugStats.group);

    bool expected = false;
    if (__atomic_compare_exchange_n(&process->debug.published, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        ARX_DEBUG_ADD(DebugStats.publishedSlots, 1);
    }

    arx_debug_group_end(&DebugStats.group);
}

void ProcessManager::DebugExit(process_t* process)
{
    arx_debug_group_begin(&DebugStats.group);

    bool expected = false;
    if (__atomic_compare_exchange_n(&process->debug.firstExited, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        ARX_DEBUG_ADD(DebugStats.firstExits, 1);
        ARX_DEBUG_ADD(DebugStats.exitedRetained, 1);
    }

    arx_debug_group_end(&DebugStats.group);
}

void ProcessManager::PublishDebug(DebugManager& manager, TaskManager* tasks, const void* vfsRoute)
{
    DebugStats.tasks = tasks;
    DebugStats.taskStats = tasks != nullptr ? &tasks->DebugStats : nullptr;
    DebugStats.vfsRoute = vfsRoute;
    const char* names[] = {"tree", "details", "files", "credentials"};

    for (size_t index = 0; index < 4; index++)
    {
        processViews[index] = {names[index], "ProcessDebugStats", "arx.process", 1,
                               &DebugStats, sizeof(DebugStats), 0, MAX_PROCESSES};
    }

    manager.Register("process", "ProcessDebugStats", &DebugStats, sizeof(DebugStats),
                     1, &DebugStats.group, processViews, 4);
}

void TaskManager::PublishDebug(DebugManager& manager, ProcessManager* processes)
{
    DebugStats.processes = processes;
    DebugStats.processStats = processes != nullptr ? &processes->DebugStats : nullptr;
    const char* names[] = {"list", "details", "files"};

    for (size_t index = 0; index < 3; index++)
    {
        taskViews[index] = {names[index], "TaskDebugStats", "arx.task", 1,
                            &DebugStats, sizeof(DebugStats), 0, MAX_TASKS};
    }

    manager.Register("task", "TaskDebugStats", &DebugStats, sizeof(DebugStats),
                     1, &DebugStats.group, taskViews, 3);
}

#endif
