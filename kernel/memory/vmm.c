/*
 * vmm.c
 *
 * Virtual Memory Manager (VMM) implementation
 *
 * - Implements architecture abstracted thread safe paging operations API
 *
 * Author: Marwan Mostafa
 *
 */

#include <memory/vmm.h>

virt_addr_t KERNEL_VIRTUAL_BASE;
virt_addr_t KERNEL_VIRTUAL_END;

virt_addr_t USER_VIRTUAL_BASE;
virt_addr_t USER_VIRTUAL_END;

size_t MAX_REGIONS;

virt_addr_space_t init_kernel_address_space = {0};

virt_region_t* alloc_region_struct(void *metadata, size_t *metadata_count)
{
   virt_region_t* region = &((virt_region_t*)metadata)[*metadata_count];
    region->next = NULL;
    region->prev = NULL;
   (*metadata_count)++; 
   return region;
}

void vmm_init(struct boot_info* boot_info)
{
    (void) boot_info;

    KERNEL_VIRTUAL_BASE = CANONICAL_KERNEL_BASE;
    KERNEL_VIRTUAL_END  = CANONICAL_KERNEL_END;

    USER_VIRTUAL_BASE   = CANONICAL_USER_BASE;
    USER_VIRTUAL_END    = CANONICAL_USER_END;

    MAX_REGIONS = REGION_METADATA_SIZE / sizeof(virt_region_t);

    init_kernel_address_space.type = VIRT_ADDR_KERNEL;
    init_kernel_address_space.pt   = arch_get_pt();
    init_kernel_address_space.lock = 0;

    init_kernel_address_space.kernel_free_regions = NULL;
    init_kernel_address_space.kernel_used_regions = NULL;
    init_kernel_address_space.kernel_region_metadata = pmm_alloc(&pmm_zone, REGION_METADATA_SIZE);
    init_kernel_address_space.kernel_regions_count = 0;

    init_kernel_address_space.user_free_regions   = NULL;
    init_kernel_address_space.user_used_regions   = NULL;
    init_kernel_address_space.user_region_metadata = pmm_alloc(&pmm_zone, REGION_METADATA_SIZE);
    init_kernel_address_space.user_regions_count = 0;

    // initial regions before removing existing mappings
    virt_region_t* initial_kernel_region = alloc_region_struct(init_kernel_address_space.kernel_region_metadata, &init_kernel_address_space.kernel_regions_count);
    initial_kernel_region->start = KERNEL_VIRTUAL_BASE;
    initial_kernel_region->end   = KERNEL_VIRTUAL_END;
    initial_kernel_region->size  = KERNEL_VIRTUAL_END - KERNEL_VIRTUAL_BASE;
    initial_kernel_region->type  = VIRT_ADDR_KERNEL;
    init_kernel_address_space.kernel_free_regions = initial_kernel_region;

    virt_region_t* initial_user_region = alloc_region_struct(init_kernel_address_space.user_region_metadata, &init_kernel_address_space.user_regions_count);
    initial_user_region->start = USER_VIRTUAL_BASE;
    initial_user_region->end   = USER_VIRTUAL_END;
    initial_user_region->size  = USER_VIRTUAL_END - USER_VIRTUAL_BASE;
    initial_user_region->type  = VIRT_ADDR_USER;
    init_kernel_address_space.user_free_regions = initial_user_region;

}

void vmm_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, virt_addr_space_t* space)
{
    spinlock_acquire(&space->lock);
    arch_map_page(va, pa, flags, space->pt);
    spinlock_release(&space->lock);
}

void vmm_unmap_page(virt_addr_t va, virt_addr_space_t* space)
{
    spinlock_acquire(&space->lock);
    arch_unmap_page(va, space->pt);
    spinlock_release(&space->lock);
}

void vmm_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, virt_addr_space_t* space)
{
    spinlock_acquire(&space->lock);
    arch_map_range(va_start, pa_start, size, flags, space->pt);
    spinlock_release(&space->lock);
}

void vmm_unmap_range(virt_addr_t va_start, uint64_t size, virt_addr_space_t* space)
{
    spinlock_acquire(&space->lock);
    arch_unmap_range(va_start, size, space->pt);
    spinlock_release(&space->lock);
}

void vmm_protect_page(virt_addr_t va, uint64_t flags, virt_addr_space_t* space)
{
    spinlock_acquire(&space->lock);
    arch_protect(va, flags, space->pt);
    spinlock_release(&space->lock);
}

void vmm_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, virt_addr_space_t* space)
{
    spinlock_acquire(&space->lock);
    arch_protect_range(va_start, size, flags, space->pt);
    spinlock_release(&space->lock);
}

void vmm_switch_addr_space(virt_addr_space_t* space)
{
    spinlock_acquire(&space->lock);
    arch_set_pt(space->pt);
    spinlock_release(&space->lock);
}

phys_addr_t vmm_virt_to_phys(virt_addr_t va, virt_addr_space_t* space)
{
    spinlock_acquire(&space->lock);
    phys_addr_t pa = arch_virt_to_phys(va, space->pt);
    spinlock_release(&space->lock);
    return pa;
}
