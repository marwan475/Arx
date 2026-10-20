#ifndef PMM_H
#define PMM_H

#include <boot/boot.h>
#include <debug/kernel/stats/PmmDebugStats.h>
#include <klib/spinlock.h>
#include <memory/heap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PMM_MAX_REGIONS 128
#define MAX_ORDER 10
#define PMM_INVALID_PFN UINT64_MAX

typedef uint64_t phys_addr_t;

typedef struct pmm_region
{
    uint64_t start_pfn;
    uint64_t end_pfn;
    size_t   page_count;
} pmm_region_t;

enum page_flags
{
    PMM_PAGE_RESERVED = 0,
    PMM_PAGE_FREE     = 1,
    PMM_PAGE_USED     = 2,
};

typedef struct page page_t;

typedef struct page
{
    uint64_t pfn;
    uint64_t flags;
    uint64_t order;
    page_t*  next;
    page_t*  prev;
} page_t;

typedef struct free_list
{
    page_t* head;
} free_list_t;

typedef struct zone
{
    spinlock_t   lock;
    pmm_region_t regions[PMM_MAX_REGIONS];
    size_t       region_count;
    size_t       total_pages;
    size_t       free_pages;
    size_t       used_pages;
    size_t       max_pfn;
    size_t       min_pfn;
    page_t*      buddy_metadata;
    free_list_t  buddy_free_lists[MAX_ORDER + 1];
    bool         hhdm_present;
    uint64_t     hhdm_offset;
    size_t       total_memory;
    ARX_DEBUG_ONLY(arx_zone_debug_stats_t debugStats;)
} zone_t;

typedef struct numa_node
{
    zone_t        zone;
    zone_t        dma_zone;
    size_t        cpu_count;
    uint32_t      proximity_domain;
    kernel_heap_t heap;
} numa_node_t;

#ifdef __cplusplus
extern "C"
{
#endif

    void pmm_init(struct boot_info* boot_info);

    // returns hhdm address of allocated block
    void* pmm_alloc(size_t size);
    void* pmm_alloc_dma(size_t size, phys_addr_t maximum_address, phys_addr_t* physical_address);
    void  pmm_free(void* addr);

#ifdef __cplusplus
}

#endif

#endif
