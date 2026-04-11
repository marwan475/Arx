#include <memory/vmm.h>

virt_addr_space_t init_kernel_address_space = {0};

void vmm_init()
{

    init_kernel_address_space.type = VIRT_ADDR_KERNEL;
    init_kernel_address_space.pt   = arch_get_pt();
    init_kernel_address_space.lock = 0;

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
