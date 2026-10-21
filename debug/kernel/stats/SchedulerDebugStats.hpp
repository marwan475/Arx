#pragma once
#include "SchedulingDebug.hpp"
#include "TaskDebugStats.hpp"

#if DEBUG

class Scheduler;

// arx.scheduler/1. Manager-lifetime borrowed roots. count=0, capacity=128
// on every view. sleepEntries casts to Scheduler::sleep_entry_t[128]; heads
// and CPU arrays have cpuCapacity=BOOT_SMP_MAX_CPUS elements, cpuCount configured.
// Tasks scan TaskDebugStats records in physical slot order; no ready-list root.
// Sleep slot is physical task slot, never TID. heads link next until sentinel;
// chains have ascending monotonic deadlines. All occupied slots (task!=null)
// include awakened/dispatched entries no longer reachable from a head.
// Read arrays/chains with shared budgets, validate every index and detect cycles.
// Borrowed task/process links require current generation validation; slot reuse
// has no history. queue group brackets QueueLock mutations, not all lifecycle or
// policy writers. Zero writers never certifies a cross-manager atomic snapshot.
// Eligibility is derived from stored values for a CPU, not a dispatch prediction:
// account/replenish and RetainForDispatch cannot be executed by an inspector.
// selectAttempts counts all RunSchedulingAlgorithm calls (including placement
// and publication); selectedTasks/selectedIdle count choices, not dispatches.
// publishAttempts counts valid-CPU publication branches; publishedTasks counts
// queued assignments, including republishing an already queued task. dispatch*
// counts RunNextReadyProcess attempts after its guards and eventual bool results,
// including idle dispatch. blocked counts inserted sleep slots, awakened
// counts queue removals by wake probes, dispatchedSleepers counts selected woken
// slots, dispatchRequeues counts failed-dispatch queued restoration. policyChanges
// counts committed ConfigurePolicy; donations counts actual effective changes.
// rescheduleRequests counts RequestReschedule calls, including a zero CPU mask.
// No counter establishes that another CPU executed work or delivered a signal.
// Time/budget/stamp fields are ns; utilization has scale=1,000,000; readyOrder
// is ordering, scheduleRuntime is weighted service, not time. Counters wrap u64.
struct SchedulerDebugStats
{
    Scheduler* source;
    const TaskDebugStats* taskStats;
    const ProcessDebugStats* processStats;
    const void* sleepEntries;
    const size_t* heads;
    const bool* active;
    task_t* const* idle;
    task_t* const* execution;
    const uint64_t* executionStamp;
    const uint64_t* deadlineUtilization;
    const uint64_t* realtimeWindow;
    const uint64_t* realtimeConsumed;
    const uint64_t* readySequence;
    uint64_t cpuCount;
    uint64_t cpuCapacity;
    uint64_t capacity;
    uint64_t sentinel;
    arx_debug_group_t group;
    uint64_t publishAttempts;
    uint64_t publishedTasks;
    uint64_t selectAttempts;
    uint64_t selectedTasks;
    uint64_t selectedIdle;
    uint64_t dispatchAttempts;
    uint64_t dispatchSuccess;
    uint64_t dispatchFailure;
    uint64_t blocked;
    uint64_t awakened;
    uint64_t dispatchedSleepers;
    uint64_t dispatchRequeues;
    uint64_t policyChanges;
    uint64_t donations;
    uint64_t rescheduleRequests;
    DebugResultStats sleeps;
};
#endif
