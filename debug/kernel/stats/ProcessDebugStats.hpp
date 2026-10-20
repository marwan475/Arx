#pragma once

#include <debug/kernel/DebugRecords.h>

#if DEBUG

class ProcessManager;
class TaskManager;
class DebugManager;
struct process_t;
struct TaskDebugStats;

// Operation totals have manager lifetime and unsigned modular overflow semantics.
// Attempts and inFlight include incomplete calls. successes denotes committed
// operations, not merely a nonnegative return; noops/refusals are separate.
struct DebugOperationStats
{
    uint64_t attempts;
    uint64_t successes;
    uint64_t failures;
    uint64_t noops;
    uint64_t refusals;
    uint64_t inFlight;
};

class DebugOperation
{
public:
    explicit DebugOperation(DebugOperationStats& stats) : Stats(stats), Finished(false)
    {
        ARX_DEBUG_ADD(Stats.attempts, 1);
        ARX_DEBUG_ADD(Stats.inFlight, 1);
    }

    ~DebugOperation()
    {
        Finish(2);
    }

    // 0 committed, 1 no change/no result, 2 failure, 3 silent refusal.
    void Finish(unsigned outcome = 0)
    {
        if (Finished)
        {
            return;
        }

        uint64_t* counter = &Stats.failures;
        if (outcome == 0)
        {
            counter = &Stats.successes;
        }
        else if (outcome == 1)
        {
            counter = &Stats.noops;
        }
        else if (outcome == 3)
        {
            counter = &Stats.refusals;
        }
        ARX_DEBUG_ADD(*counter, 1);
        ARX_DEBUG_ADD(Stats.inFlight, UINT64_MAX);
        Finished = true;
    }

private:
    DebugOperationStats& Stats;
    bool Finished;
};

class DebugUpdate
{
public:
    explicit DebugUpdate(arx_debug_group_t& group) : Group(group), Active(true)
    {
        arx_debug_group_begin(&Group);
    }

    ~DebugUpdate()
    {
        End();
    }

    void End()
    {
        if (Active)
        {
            arx_debug_group_end(&Group);
            Active = false;
        }
    }

private:
    arx_debug_group_t& Group;
    bool Active;
};

struct ProcessSlotDebug
{
    uint64_t generation;
    uint64_t parentId;
    uint64_t parentTaskId;
    uint64_t parentGeneration;
    uint64_t parentTaskGeneration;
    uint64_t parentTaskSlot;
    uint64_t recipientAddress;
    uint64_t recipientGeneration;
    bool exhausted;
    bool parentCaptured;
    bool published;
    bool firstExited;
};

// arx.process/1: manager-lifetime root, count=0, capacity=64. source->Processes
// is a fixed contiguous array in slot order; records/running/taskStats provide
// direct typed native roots without private-manager offsets. Inspect allocated
// slots, including
// initializing and exited-retained occupants. PID equals process slot; generation
// identifies an incarnation. Never equate a reused slot/address with an old owner.
// Captured parent generation must match current parent before drawing an edge.
// Generations saturate and exhausted identities are unavailable, never wrapped.
// Reclamation clears occupant metadata while retaining only generation bookkeeping.
// No process/task history survives reclamation. Shared heap objects have ephemeral
// address identities, not process generations. tasks is borrowed and may be null
// before composition binding. CPU arrays have BOOT_SMP_MAX_CPUS entries.
struct ProcessDebugStats
{
    ProcessManager* source;
    process_t* records;
    process_t* const* running;
    const TaskDebugStats* taskStats;
    TaskManager* tasks;
    const void* vfsRoute;
    const void* fileOperations[3]; // verified pipe, memory-file, socket operations
    const void* filePolls[2]; // verified event counter and epoll Poll identities
    const arx_debug_group_t* lifecycleGroup;
    const arx_debug_group_t* credentialGroup;
    arx_debug_group_t group;
    uint64_t capacity;
    uint64_t slotAllocations;
    uint64_t reclaimed;
    uint64_t liveSlots;
    uint64_t publishedSlots;
    uint64_t exitedRetained;
    uint64_t firstExits;
    uint64_t reparentings;
    uint64_t stops;
    uint64_t continues;
    uint64_t notifications;
    DebugOperationStats allocations;
    DebugOperationStats creations;
    DebugOperationStats reclamations;
};

#endif
