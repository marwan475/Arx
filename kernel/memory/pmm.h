#ifndef PMM_H
#define PMM_H

#include <boot/boot.h>
#include <klib/klib.h>

#define PMM_MAX_REGIONS 128
#define MAX_ORDER 10

#ifndef PHYS_ADDR_T_DEFINED
#define PHYS_ADDR_T_DEFINED
typedef uint64_t phys_addr_t;
#endif

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
} zone_t;

extern zone_t pmm_zone;

void pmm_init(struct boot_info* boot_info);

// returns hhdm address of allocated block
void* pmm_alloc(zone_t* zone, size_t size);
void  pmm_free(zone_t* zone, void* addr);

#endif
