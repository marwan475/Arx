#ifndef ARX_PMM_DEBUG_STATS_H
#define ARX_PMM_DEBUG_STATS_H

#include "AllocationStats.h"

#if DEBUG

struct platform;

// arx.memory.pmm/1: platform-owned, kernel-lifetime root; count=0 means
// source->numa_node_count is dynamic and bounded by capacity=MAX_NUMA_NODES.
// Nodes are contiguous, ordered by node index, never by CPU references.
// Zones own regions [start_pfn,end_pfn), not their broad min/max envelope.
// All zones borrow one absolute-PFN metadata allocation [0,metadataCapacity).
// Metadata interior flags/order may be stale: only validated heads have meaning.
// Free lists are null-terminated doubly linked page_t heads, newest first,
// bounded by metadataCapacity and owned pages. No block/PFN identity reuse
// guarantee exists. HHDM conversion is an interpretation, never a memory probe.
// ready=0 forbids traversal of partially seeded lists. Counts include seeding;
// operation totals exclude boot metadata reservation. Requests[0/1] count whole
// normal/DMA requests; zone operations count fallback attempts independently.
typedef struct arx_pmm_debug_stats
{
    struct platform* source;
    uint64_t capacity;
    uint64_t ready;
    uint64_t metadataCapacity;
    uint64_t metadataReservedBytes;
    uint64_t freeCalls;
    arx_allocation_stats_t requests[2];
} arx_pmm_debug_stats_t;

typedef struct arx_zone_debug_stats
{
    arx_debug_group_t group;
    arx_allocation_stats_t operations;
    uint64_t freeBlocks[11];
} arx_zone_debug_stats_t;

#endif
#endif
