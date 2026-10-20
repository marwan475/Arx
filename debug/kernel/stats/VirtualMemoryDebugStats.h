#ifndef ARX_VIRTUAL_MEMORY_DEBUG_STATS_H
#define ARX_VIRTUAL_MEMORY_DEBUG_STATS_H

#include <debug/kernel/DebugRecords.h>

#if DEBUG

// arx.memory.virtual/1. Platform-lifetime borrowed roots; no object catalogue.
// count=0/capacity=0: lists are null-terminated, independently budgeted and may
// be interrupted. Read used regions for ownership; free metadata can be stale.
// Region sizes/offsets are bytes; object mappings count page-region edges.
// pendingHead can be empty during retry: retryActive marks detached work.
// sharedResidentBytes is the existing maintained unique shared-page accounting,
// not total process RSS. Other resident totals are selected-space observations.
// Counters are unsigned modular lifetime totals, not transactionally coherent.
// Format fields describe the repository paging model, not probed CPU features.
typedef struct arx_vmm_debug_stats
{
    arx_debug_group_t group;
    void* kernelSpace;
    void* pendingHead;
    const uint64_t* sharedResidentBytes;
    const void* platform;
    const void* processes;
    const arx_debug_group_t* resourceGroup;
    const arx_debug_group_t* lifecycleGroup;
    uint64_t ready;
    uint64_t hhdmPresent;
    uint64_t hhdmOffset;
    uint64_t hhdmStart;
    uint64_t hhdmEnd;
    uint64_t virtualBits;
    uint64_t physicalBits;
    uint64_t levels;
    uint64_t mapAttempts;
    uint64_t mapSuccesses;
    uint64_t mapFailures;
    uint64_t unmapRequests;
    uint64_t protectRequests;
    uint64_t flushAttempts;
    uint64_t flushSuccesses;
    uint64_t flushFailures;
    uint64_t retryAttempts;
    uint64_t retryFailures;
    uint64_t retryActive;
    uint64_t retainedWritebacks;
    uint64_t objectsReclaimed;
    uint64_t pagesReclaimed;
    uint64_t cowRestored;
    uint64_t cowCopied;
} arx_vmm_debug_stats_t;

#ifdef __cplusplus
extern "C"
{
#endif
extern arx_vmm_debug_stats_t arx_vmm_debug;
#ifdef __cplusplus
}
#endif

// C cleanup scopes balance markers on every return, without runtime locks.
static inline void arx_vmm_debug_end(arx_debug_group_t** group)
{
    arx_debug_group_end(*group);
}

#define ARX_VMM_UPDATE() \
    arx_debug_group_t* debugGroup __attribute__((cleanup(arx_vmm_debug_end))) = &arx_vmm_debug.group; \
    arx_debug_group_begin(debugGroup)

#else
#define ARX_VMM_UPDATE() do { } while (0)
#endif
#endif
