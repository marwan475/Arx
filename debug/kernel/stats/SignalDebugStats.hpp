#pragma once
#include "CommunicationDebug.hpp"

#if DEBUG
class SignalManager;

// arx.signals/1. Borrowed process/task slots use ProcessDebugStats incarnation
// identities. Dispositions may be shared; require references>0 and inspect lock.
// bit n-1 means signal n (1..64); RTMIN=32. Standard senders[0..30] describe
// pending bits only. RT records are a compact array in enqueue order, not a ring:
// 0<=count, reserved, count+reserved<=128. Reservations are not pending records.
// Null queues mean no published storage. Read count/reserved before records.
// Eligibility is a stopped-state mask interpretation, never a delivery promise.
// DebugStats is class-owned static storage: totals include every SignalManager
// instance and static helper caller, including selftests, for the boot lifetime.
// API counters include recursive Send fanout calls; successes mean nonnegative
// returns, including signal-zero probes and ignored/coalesced queue submissions.
// processQueued/taskQueued count accepted nonignored Queue submissions, including
// standard coalescing; realtimeQueued counts appended records. consumed counts
// Consume calls at its existing valid-pending-signal contract, not user delivery.
// Consumed records are distinct from prepared handler frames and signalfd copies.
// reservation totals count acquired/released slots, not current queue occupancy.
// RT discards count removal by Discard, not storage freed during owner teardown.
// Atomic counters wrap modulo 2^64; group is a multiwriter interruption marker,
// not a consistency lock. Resource-owned mutations also require checking their
// provider groups and disposition locks. No target method or lock may be called.
struct SignalDebugStats
{
    SignalManager* source;
    const ProcessDebugStats* processes;
    arx_debug_group_t group;
    CommunicationOperationStats send;
    CommunicationOperationStats queue;
    CommunicationOperationStats reserve;
    uint64_t processQueued;
    uint64_t taskQueued;
    uint64_t ignored;
    uint64_t standardCoalesced;
    uint64_t realtimeQueued;
    uint64_t consumed;
    uint64_t realtimeConsumed;
    uint64_t realtimeDiscarded;
    uint64_t reservationsAcquired;
    uint64_t reservationsReleased;
    uint64_t handlerFrames;
    uint64_t handlerFailures;
    uint64_t restoredFrames;
};
#endif
