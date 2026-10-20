#pragma once
#include "ProcessDebugStats.hpp"

#if DEBUG

// Single ProcessController-owned record. Memory-ticket extensions belong here,
// not in a duplicate provider. Counters do not count temporary exec-image slots
// as published clones. Exit API requests differ from ProcessManager firstExits.
struct LifecycleDebugStats
{
    arx_debug_group_t group;
    DebugOperationStats clone;
    DebugOperationStats exec;
    DebugOperationStats exit;
    DebugOperationStats wait;
    uint64_t waitsBlocked;
    uint64_t waitsConsumed;
    uint64_t waitsObserved;
};

#endif
