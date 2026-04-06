#ifndef PMM_H
#define PMM_H

#include <boot/boot.h>
#include <klib/klib.h>

#define PMM_MAX_REGIONS 64

typedef struct pmm_region
{
    uint64_t base;
    uint64_t end;
} pmm_region_t;

typedef struct pmm_pfn_range
{
    uint64_t start_pfn;
    uint64_t end_pfn;
} pmm_pfn_range_t;

typedef struct page page_t;

typedef struct page {
    uint64_t pfn;
    uint64_t flags;
    uint64_t order;
    page_t* next;
    page_t* prev;
} page_t;

void pmm_init(struct boot_info* boot_info);

#endif