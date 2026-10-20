#ifndef ARX_ALLOCATION_STATS_H
#define ARX_ALLOCATION_STATS_H

#include <debug/kernel/DebugRecords.h>

#if DEBUG

// Lifetime totals start before allocator initialization. Requested/rounded byte
// totals count successful allocations only. Live requested sizes are not stored.
// A free is counted only when allocator metadata accepts an actual release.
// Counters are atomic observations; groups detect interrupted mutations, not
// coherent snapshots across groups or providers.
typedef struct arx_allocation_stats
{
    uint64_t attempts;
    uint64_t successes;
    uint64_t failures;
    uint64_t frees;
    uint64_t requestedBytes;
    uint64_t roundedBytes;
    uint64_t liveBlocks;
    uint64_t liveRoundedBytes;
} arx_allocation_stats_t;

static inline void arx_allocation_success(arx_allocation_stats_t* stats, uint64_t requested, uint64_t rounded)
{
    ARX_DEBUG_ADD(stats->successes, 1);
    ARX_DEBUG_ADD(stats->requestedBytes, requested);
    ARX_DEBUG_ADD(stats->roundedBytes, rounded);
    ARX_DEBUG_ADD(stats->liveBlocks, 1);
    ARX_DEBUG_ADD(stats->liveRoundedBytes, rounded);
}

static inline void arx_allocation_free(arx_allocation_stats_t* stats, uint64_t rounded)
{
    ARX_DEBUG_ADD(stats->frees, 1);
    ARX_DEBUG_ADD(stats->liveBlocks, UINT64_MAX);
    ARX_DEBUG_ADD(stats->liveRoundedBytes, (uint64_t) -rounded);
}

#endif
#endif
