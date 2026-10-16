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

            memset(region->shared, 0, sizeof(*region->shared));
            region->shared->references = 1;
            region->shared->pointer    = region->backing;
        }

        virt_region_t* copy      = FindRegion(child, region->start);
        copy->backing            = region->backing;
        copy->shared             = region->shared;
        copy->protection         = region->protection;
        copy->heap_page          = region->heap_page;
        copy->object             = region->object;
        copy->backing_offset     = region->backing_offset;
        copy->shared_mapping     = region->shared_mapping;
        copy->maximum_protection = region->maximum_protection;
        __atomic_add_fetch(&copy->shared->references, 1U, __ATOMIC_ACQ_REL);

        if (copy->object != nullptr)
        {
            vmm_retain_object(copy->object);
            __atomic_add_fetch(&copy->object->mappings, 1U, __ATOMIC_ACQ_REL);

            if (copy->shared_mapping && (copy->maximum_protection & 2) != 0)
            {
                __atomic_add_fetch(&copy->object->writable_mappings, 1U, __ATOMIC_ACQ_REL);
            }
        }

        const uint64_t flags = anonymous_page_flags(region->shared_mapping ? region->protection : region->protection & ~2U);
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
    virt_backing_object_t* object = CreateBacking(PAGE_SIZE);

    if (object == nullptr)
    {
        pmm_free(page);
        FreeRegion(space, address);

        return false;
    }
    const phys_addr_t physicalAddress = hhdm_to_pa(reinterpret_cast<uintptr_t>(page), platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
    // Intermediate tables remain writable/user-accessible; protection is leaf-only.

    if (!MapPage(address, physicalAddress, anonymous_page_flags(3), space))
    {
        pmm_free(page);
        vmm_put_object(object);
        FreeRegion(space, address);
        return false;
    }

    virt_region_t* region      = FindRegion(space, address);
    region->backing            = page;
    region->protection         = protection;
    region->heap_page          = heap;
    region->object             = object;
    region->maximum_protection = 7;
    object->mappings           = 1;
    ProtectPage(address, anonymous_page_flags(protection), space);
    return true;
}

int64_t VirtualMemoryManager::ReleaseAnonymousPage(virt_addr_space_t* space, uint64_t address, bool force)
{
    virt_region_t* region = FindRegion(space, address);

    if (region == nullptr || region->backing == nullptr)
    {
        return 0;
    }

    // Synchronous shootdown completes before physical-page reuse.
    UnmapPage(address, space);

    if (region->shared_mapping)
    {
        const int64_t status = vmm_flush_object(region->object);

        if (status != 0 && !force)
        {
            const phys_addr_t physical = hhdm_to_pa(reinterpret_cast<uintptr_t>(region->backing), platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
            MapPage(address, physical, anonymous_page_flags(region->protection), space);

            return status;
        }
    }
    vmm_release_backing(region);
    region->backing    = nullptr;
    region->protection = 0;
    region->heap_page  = false;
    FreeRegion(space, address);

    return 0;
}

void VirtualMemoryManager::ProtectAnonymousPage(virt_addr_space_t* space, uint64_t address, uint32_t protection)
{
    virt_region_t* region = FindRegion(space, address);
    const bool     shared = !region->shared_mapping && region->shared != nullptr && __atomic_load_n(&region->shared->references, __ATOMIC_ACQUIRE) > 1;
    ProtectPage(address, anonymous_page_flags(shared ? protection & ~2U : protection), space);
    region->protection = protection;
}

bool VirtualMemoryManager::DiscardAnonymousPage(virt_addr_space_t* space, uint64_t address)
{
    virt_region_t* region = FindRegion(space, address);

    if (region->shared_mapping)
    {
        return false;
    } // Logic handles shared cache advice.

    if (region->object != nullptr && region->object->file)
    {
        auto* page = GetBackingPage(region->object, region->backing_offset, false);

        if (page == nullptr)
        {
            return false;
        }

        const phys_addr_t physical = hhdm_to_pa(reinterpret_cast<uintptr_t>(page->pointer), platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);

        if (!MapPage(address, physical, anonymous_page_flags(region->protection & ~2U), space))
        {
            return false;
        }

        __atomic_add_fetch(&page->references, 1U, __ATOMIC_ACQ_REL);

        if (region->shared != nullptr)
        {
            vmm_put_page(region->shared);
        }
        else
        {
            pmm_free(region->backing);
        }

        region->shared  = page;
        region->backing = page->pointer;

        return true;
    }

    if (region->shared != nullptr && __atomic_load_n(&region->shared->references, __ATOMIC_ACQUIRE) > 1)
    {
        void* page = pmm_alloc(PAGE_SIZE);

        if (page == nullptr)
        {
            return false;
        }

        memset(page, 0, PAGE_SIZE);
        const phys_addr_t physical = hhdm_to_pa(reinterpret_cast<uintptr_t>(page), platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);

        if (!MapPage(address, physical, anonymous_page_flags(region->protection), space))
        {
            pmm_free(page);

            return false;
        }

        vmm_put_page(region->shared);
        region->shared  = nullptr;
        region->backing = page;

        return true;
    }

    memset(region->backing, 0, PAGE_SIZE);

    return true;
}

virt_backing_object_t* VirtualMemoryManager::CreateBacking(uint64_t size, bool file)
{
    auto* object = static_cast<virt_backing_object_t*>(kzalloc(sizeof(virt_backing_object_t)));

    if (object != nullptr)
    {
        object->references       = 1;
        object->size             = size;
        object->file             = file;
        object->volatile_storage = !file;
    }

    return object;
}

void VirtualMemoryManager::RetainBacking(virt_backing_object_t* object)
{
    vmm_retain_object(object);
}

void VirtualMemoryManager::ReleaseBacking(virt_backing_object_t* object)
{
    vmm_put_object(object);
}

int64_t VirtualMemoryManager::FlushBacking(virt_backing_object_t* object)
{
    return vmm_flush_object(object);
}

int64_t VirtualMemoryManager::RetryWriteback()
{
    return vmm_retry_writeback();
}

virt_shared_page_t* VirtualMemoryManager::GetBackingPage(virt_backing_object_t* object, uint64_t offset, bool create)
{
    offset = align_down(offset, PAGE_SIZE);
    spinlock_acquire_preempt(&object->lock);
    virt_shared_page_t* page = object->pages;

    while (page != nullptr && page->offset != offset)
    {
        page = page->next;
    }

    if (page == nullptr && create)
    {
        page = static_cast<virt_shared_page_t*>(kzalloc(sizeof(*page)));

        if (page != nullptr)
        {
            page->pointer = pmm_alloc(PAGE_SIZE);

            if (page->pointer == nullptr)
            {
                kfree(page);
                page = nullptr;
            }
            else
            {
                memset(page->pointer, 0, PAGE_SIZE);
                page->references = 1;
                page->offset     = offset;
                page->next       = object->pages;
                object->pages    = page;

                if (!object->file)
                {
                    vmm_account_shared_page(page);
                }
            }
        }
    }

    spinlock_release_preempt(&object->lock);

    return page;
}

bool VirtualMemoryManager::MapBackingPage(virt_addr_space_t* space, uint64_t address, virt_backing_object_t* object, uint64_t offset, uint32_t protection, bool shared, uint32_t maximumProtection)
{
    if (object == nullptr || (offset & (PAGE_SIZE - 1)) != 0 || (protection & ~maximumProtection) != 0)
    {
        return false;
    }

    if (shared && (maximumProtection & 2) != 0 && !object->volatile_storage && object->write == nullptr)
    {
        return false;
    }

    const uint64_t size = __atomic_load_n(&object->size, __ATOMIC_ACQUIRE);

    if (size == 0 || offset / PAGE_SIZE > (size - 1) / PAGE_SIZE)
    {
        return false;
    }

    auto* page = GetBackingPage(object, offset);

    if (page == nullptr || !IsPageAvailable(space, address) || !vmm_reserve_page_at(space, address))
    {
        return false;
    }

    const phys_addr_t physical = hhdm_to_pa(reinterpret_cast<uintptr_t>(page->pointer), platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);

    if (!MapPage(address, physical, anonymous_page_flags(3), space))
    {
        FreeRegion(space, address);

        return false;
    }

    auto* region               = FindRegion(space, address);
    region->backing            = page->pointer;
    region->shared             = page;
    region->object             = object;
    region->backing_offset     = offset;
    region->shared_mapping     = shared;
    region->protection         = protection;
    region->maximum_protection = maximumProtection;
    __atomic_add_fetch(&page->references, 1U, __ATOMIC_ACQ_REL);
    vmm_retain_object(object);
    __atomic_add_fetch(&object->mappings, 1U, __ATOMIC_ACQ_REL);

    if (shared && (maximumProtection & 2) != 0)
    {
        __atomic_add_fetch(&object->writable_mappings, 1U, __ATOMIC_ACQ_REL);
        // Conservative tracking covers CPU stores as well as kernel user-copy.
        spinlock_acquire_preempt(&object->lock);
        page->dirty = true;
        spinlock_release_preempt(&object->lock);
    }

    ProtectPage(address, anonymous_page_flags(shared ? protection : protection & ~2U), space);

    return true;
}

int64_t VirtualMemoryManager::ResizeVolatileBacking(virt_backing_object_t* object, uint64_t size)
{
    if (object == nullptr || !object->volatile_storage || size > UINT64_MAX - (PAGE_SIZE - 1))
    {
        return LINUX_EOPNOTSUPP;
    }

    spinlock_acquire_preempt(&object->lock);

    if (size < object->size && __atomic_load_n(&object->mappings, __ATOMIC_ACQUIRE) != 0)
    {
        spinlock_release_preempt(&object->lock);

        return LINUX_EOPNOTSUPP;
    }

    auto** link = &object->pages;

    while (*link != nullptr)
    {
        auto* page = *link;

        if (page->offset >= align_up(size, PAGE_SIZE))
        {
            *link = page->next;
            vmm_put_page(page);
            continue;
        }

        const uint64_t start = size < object->size ? size : object->size;
        const uint64_t end   = size < object->size ? align_up(size, PAGE_SIZE) : size;
        const uint64_t first = start > page->offset ? start : page->offset;
        const uint64_t last  = end < page->offset + PAGE_SIZE ? end : page->offset + PAGE_SIZE;

        if (last > first)
        {
            memset(static_cast<uint8_t*>(page->pointer) + first - page->offset, 0, last - first);
        }

        link = &page->next;
    }

    __atomic_store_n(&object->size, size, __ATOMIC_RELEASE);
    spinlock_release_preempt(&object->lock);

    return 0;
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
            ReleaseAnonymousPage(space, region->start, true);
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
