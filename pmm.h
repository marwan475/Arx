#ifndef PMM_H
#define PMM_H

#include <boot/boot.h>
#include <klib/klib.h>

#define PMM_MAX_REGIONS 128
#define MAX_ORDER 10

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

void pmm_init(struct boot_info* boot_info);
void* pmm_alloc(size_t size);
void pmm_free(void* addr);

#endif