#pragma once

#include <debug/kernel/DebugRecords.h>

#if DEBUG

class DmaManager;

// arx.memory.dma/1: root belongs to DmaManager, count=0, capacity=0 (linked
// inventory, not an array). source and descriptors live until manager destruction;
// Dispatcher invalidates borrowed providers before destroying Resource.
// The matching DmaManager.hpp exposes DebugAllocationRecord for native layouts.
// source->Allocations is newest-tracked-first, null terminated; AllocationCount
// counts all records, including cleanup-only failures. Record addresses are
// ephemeral/reused, while nonzero IDs are global and never reused. Readers must
// enforce byte/entry budgets and detect cycles/count disagreement. No traversal
// count is a trusted physical allocation bound. device.pciDevice, backend context,
// function pointers, cookie and all buffer addresses are opaque, never followed.
// Group activeWriters brackets operations, including calls outside the list lock:
// a stopped operation may be between a backend action and its observation update.
// Phase/held fields then describe last recorded progress, not proven ownership.
// Status arrays index platform_dma_status_t, not successes inferred from IDs.
struct DmaDebugStats
{
    DmaManager* source;
    arx_debug_group_t group;
    uint64_t backendKind; // 0 missing, 1 platform operations, 2 custom operations
    uint64_t allocationCalls;
    uint64_t freeCalls;
    uint64_t allocationOutcomes[7];
    uint64_t freeOutcomes[7];
    uint64_t rollbackRetained;
    uint64_t cleanupCompletions;
    uint64_t successfulRecords;
    uint64_t cleanupRecords;
    uint64_t retainedRequestedBytes;
    uint64_t retainedMappedBytes;
    uint64_t retainedReservedBytes;
};

enum class DmaDebugPhase : uint64_t
{
    Preparing,
    Live,
    RollbackRetained,
    Unmapping,
    Unmapped,
    BackingReleased
};

struct DmaAllocationDebugStats
{
    uint64_t requestedAlignment;
    uint64_t requestedBoundary;
    uint64_t effectiveAlignment;
    uint64_t effectiveBoundary;
    uint64_t physicalMask;
    uint64_t lastCleanupStatus;
    DmaDebugPhase phase;
    bool cleanupOnly;
    bool backingHeld;
    bool mappingHeld;
};

class DebugDmaOperation
{
public:
    explicit DebugDmaOperation(DmaDebugStats& stats) : Stats(stats)
    {
        arx_debug_group_begin(&Stats.group);
    }

    ~DebugDmaOperation()
    {
        arx_debug_group_end(&Stats.group);
    }

private:
    DmaDebugStats& Stats;
};

#endif
