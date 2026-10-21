#include "DebugManager.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/SynchronizationManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TimeManager.hpp"
#include <platform.h>

#if DEBUG

void Scheduler::PublishDebug(DebugManager& manager)
{
    DebugStats.source = this;
    DebugStats.taskStats = &ResourceLayerImportCaps->taskManager->DebugStats;
    DebugStats.processStats = &ResourceLayerImportCaps->processManager->DebugStats;
    DebugStats.sleepEntries = SleepQueue;
    DebugStats.heads = SleepHeads;
    DebugStats.active = SchedulingActive;
    DebugStats.idle = IdleTasks;
    DebugStats.execution = ExecutionTask;
    DebugStats.executionStamp = ExecutionStamp;
    DebugStats.deadlineUtilization = DeadlineUtilization;
    DebugStats.realtimeWindow = RealtimeWindow;
    DebugStats.realtimeConsumed = RealtimeConsumed;
    DebugStats.readySequence = &ReadySequence;
    DebugStats.cpuCount = platform.cpu_count;
    DebugStats.cpuCapacity = BOOT_SMP_MAX_CPUS;
    DebugStats.capacity = SLEEP_QUEUE_CAPACITY;
    DebugStats.sentinel = NO_SLEEPER;

    static arx_debug_view_t views[3];
    const char* names[] = {"cpus", "tasks", "sleeps"};

    for (size_t index = 0; index < 3; ++index)
    {
        views[index] = {names[index], "SchedulerDebugStats", "arx.scheduler", 1,
                        &DebugStats, sizeof(DebugStats), 0, SLEEP_QUEUE_CAPACITY};
    }

    manager.Publish("scheduler", "SchedulerDebugStats", &DebugStats, sizeof(DebugStats),
                    1, &DebugStats.group, views, 3);
}

void SynchronizationManager::PublishDebug(DebugManager& manager)
{
    DebugStats.source = this;
    DebugStats.taskStats = &Resources->taskManager->DebugStats;
    DebugStats.processStats = &Resources->processManager->DebugStats;
    DebugStats.waiters = Waiters;
    DebugStats.states = PiStates;
    DebugStats.piWaiters = PiWaiters;
    DebugStats.head = &Head;
    DebugStats.tail = &Tail;
    DebugStats.order = &PiOrder;
    DebugStats.rescheduleMask = &PriorityRescheduleMask;
    DebugStats.capacity = CAPACITY;
    DebugStats.sentinel = NONE;

    static arx_debug_view_t views[2];
    const char* names[] = {"waiters", "pi"};

    for (size_t index = 0; index < 2; ++index)
    {
        views[index] = {names[index], "SynchronizationDebugStats", "arx.sync", 1,
                        &DebugStats, sizeof(DebugStats), 0, CAPACITY};
    }

    manager.Publish("sync", "SynchronizationDebugStats", &DebugStats, sizeof(DebugStats),
                    1, &DebugStats.group, views, 2);
}

void TimeManager::PublishDebug(DebugManager& manager, const ProcessDebugStats* processes)
{
    DebugStats.processStats = processes;
    DebugStats.taskStats = processes != nullptr ? processes->taskStats : nullptr;
    static arx_debug_view_t views[2];
    const char* names[] = {"clocks", "timers"};

    for (size_t index = 0; index < 2; ++index)
    {
        views[index] = {names[index], "TimeDebugStats", "arx.time", 1,
                        &DebugStats, sizeof(DebugStats), 0, ProcessManager::MAX_PROCESSES};
    }

    manager.Register("time", "TimeDebugStats", &DebugStats, sizeof(DebugStats),
                     1, &DebugStats.group, views, 2);
}

#endif
