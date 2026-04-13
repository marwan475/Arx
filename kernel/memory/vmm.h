/*
 * vmm.h
 *
 * Virtual Memory Manager (VMM) structures and public API
 *
 * - Stores virtual address space information
 * - Architecture abstracted paging operations API
 *
 * Author: Marwan Mostafa
 *
 */

#ifndef VMM_H
#define VMM_H

#include <arch/arch.h>
#include <boot/boot.h>
#include <klib/klib.h>
#include <memory/pmm.h>

#define CANONICAL_USER_BASE UINT64_C(0x0000000000000000)
#define CANONICAL_USER_END UINT64_C(0x00007fffffffffff)
#define CANONICAL_KERNEL_BASE UINT64_C(0xffff800000000000)
#define CANONICAL_KERNEL_END UINT64_C(0xffffffffffffffff)

#define REGION_METADATA_SIZE PAGE_SIZE

typedef uint64_t virt_addr_t;

typedef enum virt_type
{
    VIRT_ADDR_KERNEL,
    VIRT_ADDR_USER,
} virt_type_t;

typedef struct virt_region virt_region_t;

typedef struct virt_region
{
    virt_addr_t    start;
    virt_addr_t    end;
    size_t         size;
    virt_type_t    type;
    bool           allocated; // used for metadata allocations
    virt_region_t* next;
    virt_region_t* prev;
} virt_region_t;

typedef struct virt_addr_space
{
    virt_type_t    type;
    phys_addr_t    pt;
    spinlock_t     lock;
    virt_region_t* kernel_free_regions;
    virt_region_t* kernel_used_regions;
    void*          kernel_region_metadata;
    size_t         kernel_regions_count;
    virt_region_t* user_free_regions;
    virt_region_t* user_used_regions;
    void*          user_region_metadata;
    size_t         user_regions_count;
} virt_addr_space_t;

extern virt_addr_space_t init_kernel_address_space;

void vmm_init(struct boot_info* boot_info);

void vmm_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, virt_addr_space_t* space);
void vmm_unmap_page(virt_addr_t va, virt_addr_space_t* space);

void vmm_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, virt_addr_space_t* space);
void vmm_unmap_range(virt_addr_t va_start, uint64_t size, virt_addr_space_t* space);

void vmm_protect_page(virt_addr_t va, uint64_t flags, virt_addr_space_t* space);
void vmm_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, virt_addr_space_t* space);

void vmm_switch_addr_space(virt_addr_space_t* space);

phys_addr_t vmm_virt_to_phys(virt_addr_t va, virt_addr_space_t* space);

virt_addr_t    vmm_reserve_region(virt_addr_space_t* space, size_t size, virt_type_t type);
void           vmm_free_region(virt_addr_space_t* space, virt_addr_t addr);
virt_region_t* vmm_find_region(virt_addr_space_t* space, virt_addr_t addr);

#endif
