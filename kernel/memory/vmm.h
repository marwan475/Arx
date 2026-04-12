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

typedef uint64_t virt_addr_t;

typedef enum virt_addr_space_type
{
    VIRT_ADDR_KERNEL,
    VIRT_ADDR_USER,
} virt_addr_space_type_t;

typedef struct virt_addr_space
{
    virt_addr_space_type_t type;
    phys_addr_t            pt;
    spinlock_t             lock;
} virt_addr_space_t;

extern virt_addr_space_t init_kernel_address_space;

void vmm_init();

void vmm_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, virt_addr_space_t* space);
void vmm_unmap_page(virt_addr_t va, virt_addr_space_t* space);

void vmm_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, virt_addr_space_t* space);
void vmm_unmap_range(virt_addr_t va_start, uint64_t size, virt_addr_space_t* space);

void vmm_protect_page(virt_addr_t va, uint64_t flags, virt_addr_space_t* space);
void vmm_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, virt_addr_space_t* space);

void vmm_switch_addr_space(virt_addr_space_t* space);

phys_addr_t vmm_virt_to_phys(virt_addr_t va, virt_addr_space_t* space);

#endif
