#ifndef ARX_HEAP_DEBUG_STATS_H
#define ARX_HEAP_DEBUG_STATS_H

#include "AllocationStats.h"

#if DEBUG

struct platform;

// arx.memory.heap/1: kernel-lifetime platform root, count=0, capacity is
// MAX_NUMA_NODES. Dynamic nodes as for PMM; readiness forbids partial init walks.
// Each heap has OBJECT_SIZE_CLASS_COUNT fixed caches in ascending byte size.
// partial/full/empty_slabs are native null-terminated doubly linked slab lists.
// Native names are NOT occupancy: derive it from total_objects/free_objects.
// Slab addresses may be reused via metadata pools; no stable allocation ID.
// objects_array spans total_objects*object_size bytes. Bitmap uses ceil(total/8)
// bytes, bit 1 means allocated. Neither bitmap nor payload conveys provenance.
// Pools have null-terminated chunk lists, data/size bounds and reusable elements.
// Walks/checks must use shared byte/entry budgets, cycle detection and bounds.
// Groups detect interrupted heap operations; existing caller locking still owns
// synchronization. No snapshot guarantee for direct unsynchronized callers.
typedef struct arx_heap_debug_stats
{
    struct platform* source;
    uint64_t capacity;
    uint64_t ready;
    uint64_t allocationCalls;
    uint64_t allocationFailures;
    uint64_t freeCalls;
} arx_heap_debug_stats_t;

typedef struct arx_cache_debug_stats
{
    arx_allocation_stats_t operations;
    uint64_t slabsInitialized;
    uint64_t slabInitFailures;
    uint64_t payloadBackingBytes;
    uint64_t bitmapBackingBytes;
    uint64_t retainedInitFailureBytes;
} arx_cache_debug_stats_t;

#endif
#endif
