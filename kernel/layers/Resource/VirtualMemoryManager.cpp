#include "layers/Resource/VirtualMemoryManager.hpp"

#include <klib/klib.h>

extern "C"
{
#include <platform.h>
}

bool VirtualMemoryManager::MapPage(virt_addr_t va, phys_addr_t pa, uint64_t flags, virt_addr_space_t* space)
{
    return vmm_map_page(va, pa, flags, space);
}

void VirtualMemoryManager::UnmapPage(virt_addr_t va, virt_addr_space_t* space)
{
    vmm_unmap_page(va, space);
}

bool VirtualMemoryManager::MapRange(virt_addr_t vaStart, phys_addr_t paStart, uint64_t size, uint64_t flags, virt_addr_space_t* space)
{
    return vmm_map_range(vaStart, paStart, size, flags, space);
}

void VirtualMemoryManager::UnmapRange(virt_addr_t vaStart, uint64_t size, virt_addr_space_t* space)
{
    vmm_unmap_range(vaStart, size, space);
}

void VirtualMemoryManager::ProtectPage(virt_addr_t va, uint64_t flags, virt_addr_space_t* space)
{
    vmm_protect_page(va, flags, space);
}

void VirtualMemoryManager::ProtectRange(virt_addr_t vaStart, uint64_t size, uint64_t flags, virt_addr_space_t* space)
{
    vmm_protect_range(vaStart, size, flags, space);
}

void VirtualMemoryManager::SwitchAddressSpace(virt_addr_space_t* space)
{
    vmm_switch_addr_space(space);
}

phys_addr_t VirtualMemoryManager::VirtToPhys(virt_addr_t va, virt_addr_space_t* space)
{
    return vmm_virt_to_phys(va, space);
}

virt_addr_t VirtualMemoryManager::ReserveRegion(virt_addr_space_t* space, size_t size, virt_type_t type)
{
    return vmm_reserve_region(space, size, type);
}

void VirtualMemoryManager::FreeRegion(virt_addr_space_t* space, virt_addr_t addr)
{
    vmm_free_region(space, addr);
}

virt_region_t* VirtualMemoryManager::FindRegion(virt_addr_space_t* space, virt_addr_t addr)
{
    return vmm_find_region(space, addr);
}

namespace
{
static uint64_t anonymous_page_flags(uint32_t protection)
{
    uint64_t flags = 0;
    ARCH_PAGE_FLAGS_INIT(flags);
    ARCH_PAGE_FLAG_SET_READ(flags);

    if (protection != 0)
    {
        ARCH_PAGE_FLAG_SET_USER(flags);
    }

    if ((protection & 2) != 0)
    {
        ARCH_PAGE_FLAG_SET_WRITE(flags);
    }

    if ((protection & 4) != 0)
    {
        ARCH_PAGE_FLAG_SET_EXEC(flags);
    }

    return flags;
}

} // namespace

bool VirtualMemoryManager::IsPageAvailable(virt_addr_space_t* space, uint64_t address)
{
    if (FindRegion(space, address) != nullptr || VirtToPhys(address, space) != 0)
    {
        return false;
    }

    for (virt_region_t* region = space->user_free_regions; region != nullptr; region = region->next)
    {
        if (address >= region->start && address + PAGE_SIZE <= region->end)
        {
            return true;
        }
    }

    return false;
}

virt_addr_space_t* VirtualMemoryManager::CloneUserAddressSpace(virt_addr_space_t* source)
{
    virt_addr_space_t* child = CreateUserAddressSpace();

    if (child == nullptr)
    {
        return nullptr;
    }

    spinlock_acquire_preempt(&source->operation_lock);
    bool complete = true;

    for (virt_region_t* region = source->user_used_regions; region != nullptr; region = region->next)
    {
        if (region->backing == nullptr || region->size != PAGE_SIZE || !vmm_reserve_page_at(child, region->start))
        {
            complete = false;
            break;
        }

        if (region->shared == nullptr)
        {
            region->shared = static_cast<virt_shared_page_t*>(kmalloc(sizeof(virt_shared_page_t)));

            if (region->shared == nullptr)
            {
                FreeRegion(child, region->start);
                complete = false;
                break;
            }

            region->shared->references = 1;
            region->shared->pointer    = region->backing;
        }

        virt_region_t* copy = FindRegion(child, region->start);
        copy->backing       = region->backing;
        copy->shared        = region->shared;
        copy->protection    = region->protection;
        copy->heap_page     = region->heap_page;
        __atomic_add_fetch(&copy->shared->references, 1U, __ATOMIC_ACQ_REL);

        const uint64_t flags = anonymous_page_flags(region->protection & ~2U);
        // Revoke the parent's writable translations before publishing the child.
        ProtectPage(region->start, flags, source);

        if (!MapPage(region->start, VirtToPhys(region->start, source), flags, child))
        {
            vmm_release_backing(copy);
            copy->backing = nullptr;
            FreeRegion(child, region->start);
            complete = false;
            break;
        }
    }

    spinlock_release_preempt(&source->operation_lock);

    if (!complete)
    {
        DestroyUserAddressSpace(child);
        return nullptr;
    }

    return child;
}

bool VirtualMemoryManager::AllocateAnonymousPage(virt_addr_space_t* space, uint64_t address, uint32_t protection, bool heap)
{
    if (!IsPageAvailable(space, address) || !vmm_reserve_page_at(space, address))
    {
        return false;
    }

    void* page = pmm_alloc(PAGE_SIZE);

    if (page == nullptr)
    {
        FreeRegion(space, address);
        return false;
    }

    memset(page, 0, PAGE_SIZE);
    const phys_addr_t physicalAddress = hhdm_to_pa(reinterpret_cast<uintptr_t>(page), platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
    // Intermediate tables remain writable/user-accessible; protection is leaf-only.

    if (!MapPage(address, physicalAddress, anonymous_page_flags(3), space))
    {
        pmm_free(page);
        FreeRegion(space, address);
        return false;
    }

    virt_region_t* region = FindRegion(space, address);
    region->backing       = page;
    region->protection    = protection;
    region->heap_page     = heap;
    ProtectPage(address, anonymous_page_flags(protection), space);
    return true;
}

void VirtualMemoryManager::ReleaseAnonymousPage(virt_addr_space_t* space, uint64_t address)
{
    virt_region_t* region = FindRegion(space, address);

    if (region == nullptr || region->backing == nullptr)
    {
        return;
    }

    // Synchronous shootdown completes before physical-page reuse.
    UnmapPage(address, space);
    vmm_release_backing(region);
    region->backing    = nullptr;
    region->protection = 0;
    region->heap_page  = false;
    FreeRegion(space, address);
}

void VirtualMemoryManager::ProtectAnonymousPage(virt_addr_space_t* space, uint64_t address, uint32_t protection)
{
    virt_region_t* region = FindRegion(space, address);
    const bool shared = region->shared != nullptr && __atomic_load_n(&region->shared->references, __ATOMIC_ACQUIRE) > 1;
    ProtectPage(address, anonymous_page_flags(shared ? protection & ~2U : protection), space);
    region->protection = protection;
}

void VirtualMemoryManager::DiscardAnonymousPage(virt_addr_space_t* space, uint64_t address)
{
    virt_region_t* region = FindRegion(space, address);

    if (region->shared != nullptr && __atomic_load_n(&region->shared->references, __ATOMIC_ACQUIRE) > 1)
    {
        void* page = pmm_alloc(PAGE_SIZE);

        if (page == nullptr)
        {
            return;
        }

        memset(page, 0, PAGE_SIZE);
        const phys_addr_t physical = hhdm_to_pa(reinterpret_cast<uintptr_t>(page), platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);

        if (!MapPage(address, physical, anonymous_page_flags(region->protection), space))
        {
            pmm_free(page);
            return;
        }

        vmm_release_backing(region);
        region->backing = page;
        return;
    }

    memset(region->backing, 0, PAGE_SIZE);
}

void VirtualMemoryManager::ReleaseAnonymousPages(virt_addr_space_t* space)
{
    if (space == nullptr)
    {
        return;
    }

    cpu_preemption_disable();
    spinlock_acquire(&space->operation_lock);
    virt_region_t* region = space->user_used_regions;

    while (region != nullptr)
    {
        virt_region_t* next = region->next;

        if (region->backing != nullptr)
        {
            ReleaseAnonymousPage(space, region->start);
        }

        region = next;
    }

    spinlock_release(&space->operation_lock);
    cpu_preemption_enable();
}

virt_addr_space_t* VirtualMemoryManager::CreateUserAddressSpace()
{
    virt_addr_space_t* space = static_cast<virt_addr_space_t*>(kmalloc(sizeof(virt_addr_space_t)));

    if (space == nullptr)
    {
        return nullptr;
    }

    memset(space, 0, sizeof(*space));
    space->type = VIRT_ADDR_USER;
    space->references = 1;
    space->pt   = arch_create_user_page_table();

    if (space->pt == 0)
    {
        kfree(space);
        return nullptr;
    }

    metadata_pool_init(&space->user_region_metadata_pool, sizeof(virt_region_t), metadata_default_elements_per_chunk(sizeof(virt_region_t)));
    virt_region_t* freeRegion = static_cast<virt_region_t*>(metadata_pool_alloc(&space->user_region_metadata_pool));

    if (freeRegion == nullptr)
    {
        arch_destroy_user_page_table(space->pt);
        kfree(space);
        return nullptr;
    }

    memset(freeRegion, 0, sizeof(*freeRegion));
    freeRegion->start         = 0x10000;
    freeRegion->end           = CANONICAL_USER_END + 1;
    freeRegion->size          = freeRegion->end - freeRegion->start;
    freeRegion->type          = VIRT_ADDR_USER;
    freeRegion->allocated     = true;
    space->user_free_regions  = freeRegion;
    space->user_regions_count = 1;
    return space;
}

void VirtualMemoryManager::DestroyUserAddressSpace(virt_addr_space_t* space)
{
    // Caller has detached this isolated root from every process/CPU.

    if (space == nullptr || space->type != VIRT_ADDR_USER)
    {
        return;
    }

    if (space->pt == arch_get_pt())
    {
        uint32_t references = __atomic_load_n(&space->references, __ATOMIC_ACQUIRE);

        while (references > 1)
        {
            if (__atomic_compare_exchange_n(&space->references, &references, references - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            {
                return;
            }
        }

        return;
    }

    if (__atomic_sub_fetch(&space->references, 1U, __ATOMIC_ACQ_REL) != 0)
    {
        return;
    }

    ReleaseAnonymousPages(space);
    // Legacy ELF/stack reservations require their own ownership-aware teardown.

    if (space->user_used_regions != nullptr)
    {
        __atomic_store_n(&space->references, 1U, __ATOMIC_RELEASE);
        return;
    }

    arch_destroy_user_page_table(space->pt);
    metadata_chunk_t* chunk = space->user_region_metadata_pool.chunks;

    while (chunk != nullptr)
    {
        metadata_chunk_t* next = chunk->next;
        pmm_free(chunk->data);
        pmm_free(chunk);
        chunk = next;
    }

    kfree(space);
}
