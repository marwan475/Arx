#pragma once
#include "SchedulingDebug.hpp"
#include "TaskDebugStats.hpp"

#if DEBUG
class SynchronizationManager;

// arx.sync/1. count=0 capacity=128; roots cast to the public nested waiter_t,
// pi_state_t, pi_waiter_t arrays. Scan physical slots, selecting used; ordinary
// queue order follows head/next to sentinel=size_t(-1), retaining slot identity.
// Tail must equal last reached slot. PI edges select PiStates[state], bounded
// by capacity, and task-slot identities. A used PI state without an owner may be
// transitional/dead. Never infer arbitrary lock deadlock from an observed cycle.
// Key=(identity,offset). object!=null means shared retained backing identity;
// otherwise private address-space identity and virtual address. Destination
// retention is independent. Keys are ephemeral, not globally persistent IDs.
// Sequence is an event counter, order a PI ordering token, bits a wake mask.
// group brackets existing Lock transactions, including priority refresh. Task
// generations and effective policy are borrowed observations from taskStats.
// API totals include internal calls (e.g. Wait joining PriorityFutex), not just
// syscalls. queued counts inserted ordinary waiters; woken counts ordinary
// WakeKeyLocked consumptions, moved counts ordinary Requeue transfers, converted
// counts RequeuePriority installations of PI edges. Conversion is separate from
// ordinary wake. donationChanges counts changed effective task donation states,
// not graph passes or dispatches. All maintained counters use atomic u64 adds.
struct SynchronizationDebugStats
{
    SynchronizationManager* source;
    const TaskDebugStats* taskStats;
    const ProcessDebugStats* processStats;
    const void* waiters;
    const void* states;
    const void* piWaiters;
    const size_t* head;
    const size_t* tail;
    const uint64_t* order;
    const uint64_t* rescheduleMask;
    uint64_t capacity;
    uint64_t sentinel;
    arx_debug_group_t group;
    DebugResultStats wait;
    DebugResultStats wake;
    DebugResultStats requeue;
    DebugResultStats wakeOperation;
    DebugResultStats priority;
    DebugResultStats requeuePriority;
    uint64_t queued;
    uint64_t woken;
    uint64_t moved;
    uint64_t converted;
    uint64_t donationChanges;
};
#endif
