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

void pmm_init(struct boot_info* boot_info);

#endif