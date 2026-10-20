#pragma once
#include "ProcessDebugStats.hpp"

#if DEBUG

struct task_t;

struct TaskSlotDebug
{
    uint64_t generation;
    uint64_t processAddress;
    uint64_t processGeneration;
    bool exhausted;
    bool published;
};

// arx.task/1: count=0, capacity=128. source->Tasks is a fixed contiguous array
// ordered by physical slot. TID is mutable (exec) and is NOT always its slot.
// Slot+generation identifies an incarnation; processGeneration validates its
// borrowed process link. RunningTasks and SwitchingTasks are observed CPU links,
// distinct from task.ownerCpu and dispatchUsers. Reused addresses imply no history.
struct TaskDebugStats
{
    TaskManager* source;
    task_t* records;
    task_t* const* running;
    task_t* const* switching;
    const ProcessDebugStats* processStats;
    ProcessManager* processes;
    arx_debug_group_t group;
    uint64_t capacity;
    uint64_t slotAllocations;
    uint64_t reclaimed;
    uint64_t liveSlots;
    uint64_t identityChanges;
    DebugOperationStats allocations;
    DebugOperationStats creations;
    DebugOperationStats reclamations;
};

#endif
