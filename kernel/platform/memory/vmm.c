#include <klib/intrusive_list.h>
#include <memory/vmm.h>
#include <platform.h>
#include <debug/kernel/stats/VirtualMemoryDebugStats.h>

virt_addr_t KERNEL_VIRTUAL_BASE;
virt_addr_t KERNEL_VIRTUAL_END;

virt_addr_t USER_VIRTUAL_BASE;
virt_addr_t USER_VIRTUAL_END;

static virt_addr_space_t init_kernel_address_space = {0};

extern void dispatcher_account_minor_fault(virt_addr_space_t* space);

static spinlock_t             pending_writeback_lock;
static virt_backing_object_t* pending_writeback;
static uint64_t              shared_resident_bytes;

#if DEBUG
arx_vmm_debug_stats_t arx_vmm_debug = {
    .kernelSpace = &init_kernel_address_space,
    .sharedResidentBytes = &shared_resident_bytes,
    .platform = &platform,
};
#endif

void vmm_account_shared_page(virt_shared_page_t* page)
{
    ARX_VMM_UPDATE();
    // Called once before publication of an anonymous shared backing page.
    page->shared_accounting = true;
    __atomic_fetch_add(&shared_resident_bytes, PAGE_SIZE, __ATOMIC_RELAXED);
}

uint64_t vmm_shared_memory_bytes(void)
{
    return __atomic_load_n(&shared_resident_bytes, __ATOMIC_RELAXED);
}

void vmm_retain_object(virt_backing_object_t* object)
{
    if (object != NULL)
    {
        __atomic_add_fetch(&object->references, 1U, __ATOMIC_ACQ_REL);
    }
}

void vmm_put_page(virt_shared_page_t* page)
{
    ARX_VMM_UPDATE();
    if (page != NULL && __atomic_sub_fetch(&page->references, 1U, __ATOMIC_ACQ_REL) == 0)
    {
        if (page->shared_accounting)
        {
            __atomic_fetch_sub(&shared_resident_bytes, PAGE_SIZE, __ATOMIC_RELAXED);
        }

        ARX_DEBUG_ADD(arx_vmm_debug.pagesReclaimed, 1);
        pmm_free(page->pointer);
        kfree(page);
    }
}

int64_t vmm_flush_object(virt_backing_object_t* object)
{
    ARX_VMM_UPDATE();
    ARX_DEBUG_ADD(arx_vmm_debug.flushAttempts, 1);
    if (object == NULL || object->volatile_storage)
    {
        ARX_DEBUG_ADD(arx_vmm_debug.flushSuccesses, 1);
        return 0;
    }

    spinlock_acquire_preempt(&object->lock);
    int64_t result = 0;

    for (virt_shared_page_t* page = object->pages; page != NULL; page = page->next)
    {
        if (!page->dirty || page->offset >= object->size)
        {
            continue;
        }

        if (!dispatcher_backing_can_write(object->operations))
        {
            result = -95;
            break;
        }

        uint64_t bytes = object->size - page->offset;

        if (bytes > PAGE_SIZE)
        {
            bytes = PAGE_SIZE;
        }

        int64_t written = dispatcher_write_backing(object->operations, object->context, page->offset, page->pointer, bytes);

        if (written != (int64_t) bytes)
        {
            result = written < 0 ? written : -5;
            break;
        }

        // A writable alias may store again without faulting. Keep it dirty
        // until all such aliases have gone; never infer cleanliness from a copy.
        page->dirty = __atomic_load_n(&object->writable_mappings, __ATOMIC_ACQUIRE) != 0;
    }

    spinlock_release_preempt(&object->lock);

    ARX_DEBUG_ADD(arx_vmm_debug.flushSuccesses, result == 0 ? 1 : 0);
    ARX_DEBUG_ADD(arx_vmm_debug.flushFailures, result == 0 ? 0 : 1);
    return result;
}

void vmm_put_object(virt_backing_object_t* object)
{
    ARX_VMM_UPDATE();
    if (object == NULL || __atomic_sub_fetch(&object->references, 1U, __ATOMIC_ACQ_REL) != 0)
    {
        return;
    }

    if (vmm_flush_object(object) != 0)
    {
        // Keep failed writeback reachable and retryable after the last mapping.
        object->references = 1;
        spinlock_acquire_preempt(&pending_writeback_lock);
        object->pending_next = pending_writeback;
        pending_writeback    = object;
        ARX_DEBUG_RUN(arx_vmm_debug.pendingHead = pending_writeback);
        ARX_DEBUG_ADD(arx_vmm_debug.retainedWritebacks, 1);
        spinlock_release_preempt(&pending_writeback_lock);
        return;
    }

    virt_shared_page_t* page = object->pages;

    while (page != NULL)
    {
        virt_shared_page_t* next = page->next;
        vmm_put_page(page);
        page = next;
    }

    dispatcher_release_backing_context(object->operations, object->context);

    ARX_DEBUG_ADD(arx_vmm_debug.objectsReclaimed, 1);
    kfree(object);
}

int64_t vmm_retry_writeback(void)
{
    ARX_VMM_UPDATE();
    ARX_DEBUG_ADD(arx_vmm_debug.retryAttempts, 1);
    spinlock_acquire_preempt(&pending_writeback_lock);
    virt_backing_object_t* list = pending_writeback;
    pending_writeback           = NULL;
    ARX_DEBUG_RUN(arx_vmm_debug.pendingHead = NULL);
    ARX_DEBUG_ADD(arx_vmm_debug.retryActive, 1);
    spinlock_release_preempt(&pending_writeback_lock);
    int64_t result = 0;

    while (list != NULL)
    {
        virt_backing_object_t* next   = list->pending_next;
        int64_t                status = vmm_flush_object(list);

        if (status != 0)
        {
            result = status;
        }

        vmm_put_object(list);
        list = next;
    }

    ARX_DEBUG_ADD(arx_vmm_debug.retryFailures, result == 0 ? 0 : 1);
    ARX_DEBUG_ADD(arx_vmm_debug.retryActive, UINT64_MAX);
    return result;
}

void vmm_release_backing(virt_region_t* region)
{
    ARX_VMM_UPDATE();
    if (region->shared != NULL)
    {
        vmm_put_page(region->shared);
        region->shared = NULL;
    }
    else if (region->backing != NULL)
    {
        pmm_free(region->backing);
    }

    if (region->object != NULL)
    {
        __atomic_sub_fetch(&region->object->mappings, 1U, __ATOMIC_ACQ_REL);

        if (region->shared_mapping && (region->maximum_protection & 2) != 0)
        {
            __atomic_sub_fetch(&region->object->writable_mappings, 1U, __ATOMIC_ACQ_REL);
        }

        vmm_put_object(region->object);
        region->object = NULL;
    }
}

bool vmm_resolve_cow_locked(virt_addr_space_t* space, virt_addr_t address)
{
    ARX_VMM_UPDATE();
    virt_region_t* region = vmm_find_region(space, address);

    if (region == NULL || region->shared == NULL || (region->protection & 2) == 0)
    {
        return false;
    }

    if (region->shared_mapping)
    {
        return true;
    }

    uint64_t flags = 0;
    ARCH_PAGE_FLAGS_INIT(flags);
    ARCH_PAGE_FLAG_SET_READ(flags);
    ARCH_PAGE_FLAG_SET_WRITE(flags);
    ARCH_PAGE_FLAG_SET_USER(flags);
    if (region->protection & 4)
    {
        ARCH_PAGE_FLAG_SET_EXEC(flags);
    }

    if (__atomic_load_n(&region->shared->references, __ATOMIC_ACQUIRE) == 1)
    {
        const bool fault = !arch_user_page_accessible(region->start, space->pt, true);
        vmm_protect_page(region->start, flags, space);

        if (fault)
        {
            dispatcher_account_minor_fault(space);
        }

        ARX_DEBUG_ADD(arx_vmm_debug.cowRestored, 1);
        return true;
    }

    void* page = pmm_alloc(PAGE_SIZE);

    if (page == NULL)
    {
        return false;
    }

    memcpy(page, region->backing, PAGE_SIZE);
    phys_addr_t physical = hhdm_to_pa((uintptr_t) page, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);

    if (!vmm_map_page(region->start, physical, flags, space))
    {
        pmm_free(page);
        return false;
    }

    vmm_put_page(region->shared);
    region->shared  = NULL;
    region->backing = page;
    ARX_DEBUG_ADD(arx_vmm_debug.cowCopied, 1);
    dispatcher_account_minor_fault(space);
    return true;
}

bool vmm_handle_cow_fault(virt_addr_space_t* space, virt_addr_t address)
{
    if (space == NULL || address > CANONICAL_USER_END)
    {
        return false;
    }

    spinlock_acquire_preempt(&space->operation_lock);
    bool result = vmm_resolve_cow_locked(space, address);
    spinlock_release_preempt(&space->operation_lock);
    return result;
}

static virt_region_t* alloc_region_struct(metadata_pool_t* metadata_pool, size_t* metadata_count)
{
    if (metadata_pool == NULL || metadata_count == NULL)
    {
        return NULL;
    }

    virt_region_t* region = (virt_region_t*) metadata_pool_alloc(metadata_pool);

    if (region == NULL)
    {
        return NULL;
    }

    (*metadata_count)++;
    memset(region, 0, sizeof(*region));
    region->allocated = true;
    ILIST_NODE_INIT(region);
    return region;
}

static void free_region_struct(metadata_pool_t* metadata_pool, virt_region_t* region, size_t* metadata_count)
{
    if (metadata_pool == NULL || region == NULL || !region->allocated)
    {
        return;
    }

    region->allocated = false;
    metadata_pool_free(metadata_pool, region);
    (*metadata_count)--;
}

static void add_to_regions_list(virt_region_t* new_region, virt_region_t** list_head)
{
    ILIST_PUSH_FRONT(*list_head, new_region);
}

static void remove_from_regions_list(virt_region_t* region, virt_region_t** list_head)
{
    ILIST_REMOVE(*list_head, region);
}

static virt_region_t* handle_full_cover_case(virt_region_t* free, virt_region_t** free_regions, metadata_pool_t* metadata_pool, size_t* metadata_count, bool* stop)
{
    virt_region_t* next_free = free->next;
    *stop                    = false;

    remove_from_regions_list(free, free_regions);
    free_region_struct(metadata_pool, free, metadata_count);

    if (*free_regions == NULL)
    {
        *stop = true;
    }

    return next_free;
}

static void handle_left_trim_case(virt_region_t* free, virt_addr_t overlap_end)
{
    free->start = overlap_end;
    free->size  = free->end - free->start;
}

static void handle_right_trim_case(virt_region_t* free, virt_addr_t overlap_start)
{
    free->end  = overlap_start;
    free->size = free->end - free->start;
}

static void handle_middle_split_case(virt_region_t* free, virt_region_t** free_regions, virt_addr_t overlap_start, virt_addr_t overlap_end, metadata_pool_t* metadata_pool, size_t* metadata_count)
{
    virt_addr_t old_end = free->end;
    free->end           = overlap_start;
    free->size          = free->end - free->start;

    virt_region_t* right_region = alloc_region_struct(metadata_pool, metadata_count);

    if (right_region == NULL)
    {
        panic();
    }

    right_region->start = overlap_end;
    right_region->end   = old_end;
    right_region->size  = right_region->end - right_region->start;
    right_region->type  = free->type;

    if (free->next != NULL)
    {
        ILIST_INSERT_BEFORE(*free_regions, free->next, right_region);
    }
    else
    {
        ILIST_APPEND(*free_regions, right_region);
    }
}

static void remove_existing_mappings(virt_region_t** free_regions, virt_region_t* used_regions, metadata_pool_t* metadata_pool, size_t* metadata_count)
{
    if (free_regions == NULL || *free_regions == NULL || used_regions == NULL || metadata_pool == NULL || metadata_count == NULL)
    {
        return;
    }

    ILIST_FOR_EACH(used, used_regions)
    {
        ILIST_FOR_EACH_SAFE(free, next_free, *free_regions)
        {
            virt_addr_t overlap_start = free->start > used->start ? free->start : used->start;
            virt_addr_t overlap_end   = free->end < used->end ? free->end : used->end;

            if (overlap_start >= overlap_end)
            {
                continue;
            }

            bool touches_left  = overlap_start == free->start;
            bool touches_right = overlap_end == free->end;

            if (touches_left && touches_right)
            {
                bool stop = false;
                handle_full_cover_case(free, free_regions, metadata_pool, metadata_count, &stop);

                if (stop)
                {
                    break;
                }

                continue;
            }

            if (touches_left)
            {
                handle_left_trim_case(free, overlap_end);
                continue;
            }

            if (touches_right)
            {
                handle_right_trim_case(free, overlap_start);
                continue;
            }

            handle_middle_split_case(free, free_regions, overlap_start, overlap_end, metadata_pool, metadata_count);
        }
    }
}

void vmm_init(struct boot_info* boot_info)
{
    ARX_DEBUG_RUN(
        arx_vmm_debug.hhdmPresent = boot_info->hhdm_present;
        arx_vmm_debug.hhdmOffset = boot_info->hhdm_offset;
        arx_vmm_debug.hhdmStart = boot_info->hhdm_start;
        arx_vmm_debug.hhdmEnd = boot_info->hhdm_end;
        arch_debug_paging_format(&arx_vmm_debug.levels, &arx_vmm_debug.virtualBits, &arx_vmm_debug.physicalBits);
    );

    KERNEL_VIRTUAL_BASE = CANONICAL_KERNEL_BASE;
    KERNEL_VIRTUAL_END  = CANONICAL_KERNEL_END;

    USER_VIRTUAL_BASE = CANONICAL_USER_BASE;
    USER_VIRTUAL_END  = CANONICAL_USER_END;

    init_kernel_address_space.type = VIRT_ADDR_KERNEL;
    init_kernel_address_space.pt   = arch_get_pt();
    init_kernel_address_space.lock = 0;

    init_kernel_address_space.kernel_free_regions = NULL;
    init_kernel_address_space.kernel_used_regions = NULL;
    metadata_pool_init(&init_kernel_address_space.kernel_region_metadata_pool, sizeof(virt_region_t), metadata_default_elements_per_chunk(sizeof(virt_region_t)));
    init_kernel_address_space.kernel_regions_count = 0;

    init_kernel_address_space.user_free_regions = NULL;
    init_kernel_address_space.user_used_regions = NULL;
    metadata_pool_init(&init_kernel_address_space.user_region_metadata_pool, sizeof(virt_region_t), metadata_default_elements_per_chunk(sizeof(virt_region_t)));
    init_kernel_address_space.user_regions_count = 0;

    // initial regions before removing existing mappings
    virt_region_t* initial_kernel_region          = alloc_region_struct(&init_kernel_address_space.kernel_region_metadata_pool, &init_kernel_address_space.kernel_regions_count);
    initial_kernel_region->start                  = KERNEL_VIRTUAL_BASE;
    initial_kernel_region->end                    = KERNEL_VIRTUAL_END;
    initial_kernel_region->size                   = KERNEL_VIRTUAL_END - KERNEL_VIRTUAL_BASE;
    initial_kernel_region->type                   = VIRT_ADDR_KERNEL;
    init_kernel_address_space.kernel_free_regions = initial_kernel_region;

    // existing regions
    virt_region_t* existing_kernel_region         = alloc_region_struct(&init_kernel_address_space.kernel_region_metadata_pool, &init_kernel_address_space.kernel_regions_count);
    existing_kernel_region->start                 = boot_info->kernel_start;
    existing_kernel_region->end                   = boot_info->kernel_end;
    existing_kernel_region->size                  = boot_info->kernel_end - boot_info->kernel_start;
    existing_kernel_region->type                  = VIRT_ADDR_KERNEL;
    init_kernel_address_space.kernel_used_regions = existing_kernel_region;

    virt_region_t* existing_hhdm_region = alloc_region_struct(&init_kernel_address_space.kernel_region_metadata_pool, &init_kernel_address_space.kernel_regions_count);
    existing_hhdm_region->start         = boot_info->hhdm_present ? boot_info->hhdm_start : 0;
    existing_hhdm_region->end           = boot_info->hhdm_present ? boot_info->hhdm_end : 0;
    existing_hhdm_region->size          = boot_info->hhdm_present ? (boot_info->hhdm_end - boot_info->hhdm_start) : 0;
    existing_hhdm_region->type          = VIRT_ADDR_KERNEL;

    if (boot_info->hhdm_present)
    {
        add_to_regions_list(existing_hhdm_region, &init_kernel_address_space.kernel_used_regions);
    }

    virt_region_t* existing_framebuffer_region = alloc_region_struct(&init_kernel_address_space.kernel_region_metadata_pool, &init_kernel_address_space.kernel_regions_count);
    existing_framebuffer_region->start         = boot_info->framebuffer_addr;
    existing_framebuffer_region->end           = boot_info->framebuffer_addr + boot_info->framebuffer_width * boot_info->framebuffer_height * (boot_info->framebuffer_bpp / 8);
    existing_framebuffer_region->size          = boot_info->framebuffer_width * boot_info->framebuffer_height * (boot_info->framebuffer_bpp / 8);
    existing_framebuffer_region->type          = VIRT_ADDR_KERNEL;
    add_to_regions_list(existing_framebuffer_region, &init_kernel_address_space.kernel_used_regions);

    remove_existing_mappings(&init_kernel_address_space.kernel_free_regions, init_kernel_address_space.kernel_used_regions, &init_kernel_address_space.kernel_region_metadata_pool, &init_kernel_address_space.kernel_regions_count);

    virt_region_t* initial_user_region          = alloc_region_struct(&init_kernel_address_space.user_region_metadata_pool, &init_kernel_address_space.user_regions_count);
    initial_user_region->start                  = USER_VIRTUAL_BASE;
    initial_user_region->end                    = USER_VIRTUAL_END;
    initial_user_region->size                   = USER_VIRTUAL_END - USER_VIRTUAL_BASE;
    initial_user_region->type                   = VIRT_ADDR_USER;
    init_kernel_address_space.user_free_regions = initial_user_region;

    kprintf("Arx kernel: virtual memory manager initialized\n");

    uint8_t num_cpus = platform.cpu_count;

    for (uint8_t i = 0; i < num_cpus; i++)
    {
        platform.cpus[i].address_space = &init_kernel_address_space;
    }
    ARX_DEBUG_STORE(arx_vmm_debug.ready, 1);
}

virt_addr_space_t* vmm_kernel_space(void)
{
    return &init_kernel_address_space;
}

uint64_t vmm_kernel_page_flags(bool uncached, bool global)
{
    uint64_t flags = 0;
    ARCH_PAGE_FLAGS_INIT(flags);
    ARCH_PAGE_FLAG_SET_READ(flags);
    ARCH_PAGE_FLAG_SET_WRITE(flags);

    if (uncached)
    {
        ARCH_PAGE_FLAG_SET_NOCACHE(flags);
    }

    if (global)
    {
        ARCH_PAGE_FLAG_SET_GLOBAL(flags);
    }

    return flags;
}

virt_addr_t vmm_map_direct_mmio_page(phys_addr_t page, virt_addr_space_t* space)
{
    const zone_t*     zone    = &platform.cpus[arch_cpu_id()].numa_node->zone;
    const virt_addr_t address = pa_to_hhdm(page, zone->hhdm_present, zone->hhdm_offset);

    if (!vmm_map_page(address, page, vmm_kernel_page_flags(true, true), space))
    {
        return 0;
    }

    return address;
}

bool vmm_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, virt_addr_space_t* space)
{
    ARX_VMM_UPDATE();
    ARX_DEBUG_ADD(arx_vmm_debug.mapAttempts, 1);
    if (va >= CANONICAL_KERNEL_BASE)
    {
        space = vmm_kernel_space();
    }

    spinlock_acquire_preempt(&space->lock);
    bool success = arch_map_page(va, pa, flags, space->pt);
    spinlock_release_preempt(&space->lock);
    ARX_DEBUG_ADD(arx_vmm_debug.mapSuccesses, success ? 1 : 0);
    ARX_DEBUG_ADD(arx_vmm_debug.mapFailures, success ? 0 : 1);
    return success;
}

void vmm_unmap_page(virt_addr_t va, virt_addr_space_t* space)
{
    ARX_VMM_UPDATE();
    ARX_DEBUG_ADD(arx_vmm_debug.unmapRequests, 1);
    if (va >= CANONICAL_KERNEL_BASE)
    {
        space = vmm_kernel_space();
    }

    spinlock_acquire_preempt(&space->lock);
    arch_unmap_page(va, space->pt);
    spinlock_release_preempt(&space->lock);
}

bool vmm_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, virt_addr_space_t* space)
{
    ARX_VMM_UPDATE();
    ARX_DEBUG_ADD(arx_vmm_debug.mapAttempts, 1);
    if (va_start >= CANONICAL_KERNEL_BASE)
    {
        space = vmm_kernel_space();
    }

    spinlock_acquire_preempt(&space->lock);
    bool success = arch_map_range(va_start, pa_start, size, flags, space->pt);
    spinlock_release_preempt(&space->lock);
    ARX_DEBUG_ADD(arx_vmm_debug.mapSuccesses, success ? 1 : 0);
    ARX_DEBUG_ADD(arx_vmm_debug.mapFailures, success ? 0 : 1);
    return success;
}

void vmm_unmap_range(virt_addr_t va_start, uint64_t size, virt_addr_space_t* space)
{
    ARX_VMM_UPDATE();
    ARX_DEBUG_ADD(arx_vmm_debug.unmapRequests, 1);
    if (va_start >= CANONICAL_KERNEL_BASE)
    {
        space = vmm_kernel_space();
    }

    spinlock_acquire_preempt(&space->lock);
    arch_unmap_range(va_start, size, space->pt);
    spinlock_release_preempt(&space->lock);
}

void vmm_protect_page(virt_addr_t va, uint64_t flags, virt_addr_space_t* space)
{
    ARX_VMM_UPDATE();
    ARX_DEBUG_ADD(arx_vmm_debug.protectRequests, 1);
    if (va >= CANONICAL_KERNEL_BASE)
    {
        space = vmm_kernel_space();
    }

    spinlock_acquire_preempt(&space->lock);
    arch_protect(va, flags, space->pt);
    spinlock_release_preempt(&space->lock);
}

void vmm_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, virt_addr_space_t* space)
{
    ARX_VMM_UPDATE();
    ARX_DEBUG_ADD(arx_vmm_debug.protectRequests, 1);
    if (va_start >= CANONICAL_KERNEL_BASE)
    {
        space = vmm_kernel_space();
    }

    spinlock_acquire_preempt(&space->lock);
    arch_protect_range(va_start, size, flags, space->pt);
    spinlock_release_preempt(&space->lock);
}

void vmm_switch_addr_space(virt_addr_space_t* space)
{
    // Loading the local root does not mutate page tables. Taking the table lock
    // in an interrupt handler could block the IPI needed by its current owner.
    arch_set_pt(space->pt);
}

phys_addr_t vmm_virt_to_phys(virt_addr_t va, virt_addr_space_t* space)
{
    if (va >= CANONICAL_KERNEL_BASE)
    {
        space = vmm_kernel_space();
    }

    spinlock_acquire_preempt(&space->lock);
    phys_addr_t pa = arch_virt_to_phys(va, space->pt);
    spinlock_release_preempt(&space->lock);
    return pa;
}

virt_addr_t vmm_reserve_region(virt_addr_space_t* space, size_t size, virt_type_t type)
{
    ARX_VMM_UPDATE();
    if (type == VIRT_ADDR_KERNEL)
    {
        space = vmm_kernel_space();
    }

    if (space == NULL || size == 0)
    {
        return 0;
    }

    size_t aligned_size = align_up(size, PAGE_SIZE);

    if (aligned_size == 0)
    {
        return 0;
    }

    virt_region_t**  free_regions   = NULL;
    virt_region_t**  used_regions   = NULL;
    metadata_pool_t* metadata_pool  = NULL;
    size_t*          metadata_count = NULL;

    if (type == VIRT_ADDR_KERNEL)
    {
        free_regions   = &space->kernel_free_regions;
        used_regions   = &space->kernel_used_regions;
        metadata_pool  = &space->kernel_region_metadata_pool;
        metadata_count = &space->kernel_regions_count;
    }
    else
    {
        free_regions   = &space->user_free_regions;
        used_regions   = &space->user_used_regions;
        metadata_pool  = &space->user_region_metadata_pool;
        metadata_count = &space->user_regions_count;
    }

    spinlock_acquire_preempt(&space->lock);

    ILIST_FOR_EACH(free, *free_regions)
    {
        if (free->size < aligned_size)
        {
            continue;
        }

        virt_addr_t reserved_start = free->start;

        if (free->size == aligned_size)
        {
            remove_from_regions_list(free, free_regions);

            free->start = reserved_start;
            free->end   = reserved_start + aligned_size;
            free->size  = aligned_size;
            free->type  = type;

            add_to_regions_list(free, used_regions);
            spinlock_release_preempt(&space->lock);
            return reserved_start;
        }

        virt_region_t* used = alloc_region_struct(metadata_pool, metadata_count);

        if (used == NULL)
        {
            spinlock_release_preempt(&space->lock);
            return 0;
        }

        used->start = reserved_start;
        used->end   = reserved_start + aligned_size;
        used->size  = aligned_size;
        used->type  = type;
        ILIST_NODE_INIT(used);

        free->start += aligned_size;
        free->size = free->end - free->start;

        add_to_regions_list(used, used_regions);
        spinlock_release_preempt(&space->lock);
        return reserved_start;
    }

    spinlock_release_preempt(&space->lock);
    return 0;
}

void vmm_free_region(virt_addr_space_t* space, virt_addr_t addr)
{
    ARX_VMM_UPDATE();
    if (addr >= CANONICAL_KERNEL_BASE)
    {
        space = vmm_kernel_space();
    }

    if (space == NULL || addr == 0)
    {
        return;
    }

    spinlock_acquire_preempt(&space->lock);

    virt_region_t*   region         = NULL;
    virt_region_t**  used_regions   = NULL;
    virt_region_t**  free_regions   = NULL;
    metadata_pool_t* metadata_pool  = NULL;
    size_t*          metadata_count = NULL;

    ILIST_FOR_EACH(it, space->kernel_used_regions)
    {
        if (it->start == addr)
        {
            region         = it;
            used_regions   = &space->kernel_used_regions;
            free_regions   = &space->kernel_free_regions;
            metadata_pool  = &space->kernel_region_metadata_pool;
            metadata_count = &space->kernel_regions_count;
            break;
        }
    }

    if (region == NULL)
    {
        ILIST_FOR_EACH(it, space->user_used_regions)
        {
            if (it->start == addr)
            {
                region         = it;
                used_regions   = &space->user_used_regions;
                free_regions   = &space->user_free_regions;
                metadata_pool  = &space->user_region_metadata_pool;
                metadata_count = &space->user_regions_count;
                break;
            }
        }
    }

    if (region == NULL)
    {
        spinlock_release_preempt(&space->lock);
        return;
    }

    remove_from_regions_list(region, used_regions);

    virt_region_t* insert_before = *free_regions;

    while (insert_before != NULL && insert_before->start < region->start)
    {
        insert_before = insert_before->next;
    }

    if (insert_before == NULL)
    {
        ILIST_APPEND(*free_regions, region);
    }
    else
    {
        ILIST_INSERT_BEFORE(*free_regions, insert_before, region);
    }

    if (region->prev != NULL && region->prev->end == region->start)
    {
        virt_region_t* left = region->prev;
        left->end           = region->end;
        left->size          = left->end - left->start;
        ILIST_REMOVE(*free_regions, region);
        free_region_struct(metadata_pool, region, metadata_count);
        region = left;
    }

    if (region->next != NULL && region->end == region->next->start)
    {
        virt_region_t* right = region->next;
        region->end          = right->end;
        region->size         = region->end - region->start;
        ILIST_REMOVE(*free_regions, right);
        free_region_struct(metadata_pool, right, metadata_count);
    }

    spinlock_release_preempt(&space->lock);
}

virt_region_t* vmm_find_region(virt_addr_space_t* space, virt_addr_t addr)
{
    if (addr >= CANONICAL_KERNEL_BASE)
    {
        space = vmm_kernel_space();
    }

    if (space == NULL || addr == 0)
    {
        return NULL;
    }

    spinlock_acquire_preempt(&space->lock);

    ILIST_FOR_EACH(it, space->kernel_used_regions)
    {
        if (it->start <= addr && addr < it->end)
        {
            spinlock_release_preempt(&space->lock);
            return it;
        }
    }

    ILIST_FOR_EACH(it, space->user_used_regions)
    {
        if (it->start <= addr && addr < it->end)
        {
            spinlock_release_preempt(&space->lock);
            return it;
        }
    }

    spinlock_release_preempt(&space->lock);
    return NULL;
}

// Caller holds operation_lock; the allocator lock protects the free/used lists.
bool vmm_reserve_page_at(virt_addr_space_t* space, virt_addr_t addr)
{
    ARX_VMM_UPDATE();
    if (space == NULL || addr == 0 || (addr & (PAGE_SIZE - 1)) != 0 || addr > CANONICAL_USER_END - PAGE_SIZE + 1)
    {
        return false;
    }

    spinlock_acquire_preempt(&space->lock);
    ILIST_FOR_EACH(free, space->user_free_regions)
    {
        if (addr < free->start || addr + PAGE_SIZE > free->end)
        {
            continue;
        }

        virt_region_t* used  = alloc_region_struct(&space->user_region_metadata_pool, &space->user_regions_count);
        virt_region_t* right = NULL;
        const bool     split = addr > free->start && addr + PAGE_SIZE < free->end;

        if (used != NULL && split)
        {
            right = alloc_region_struct(&space->user_region_metadata_pool, &space->user_regions_count);
        }

        // Acquire all metadata before mutating the free interval.

        if (used == NULL || (split && right == NULL))
        {
            if (used != NULL)
            {
                free_region_struct(&space->user_region_metadata_pool, used, &space->user_regions_count);
            }

            spinlock_release_preempt(&space->lock);
            return false;
        }

        used->start = addr;
        used->end   = addr + PAGE_SIZE;
        used->size  = PAGE_SIZE;
        used->type  = VIRT_ADDR_USER;

        if (right != NULL)
        {
            right->start = used->end;
            right->end   = free->end;
            right->size  = right->end - right->start;
            right->type  = VIRT_ADDR_USER;

            if (free->next != NULL)
            {
                ILIST_INSERT_BEFORE(space->user_free_regions, free->next, right);
            }
            else
            {
                ILIST_APPEND(space->user_free_regions, right);
            }

            free->end = addr;
        }
        else if (addr == free->start)
        {
            free->start += PAGE_SIZE;
        }
        else
        {
            free->end = addr;
        }

        free->size = free->end - free->start;

        if (free->size == 0)
        {
            remove_from_regions_list(free, &space->user_free_regions);
            free_region_struct(&space->user_region_metadata_pool, free, &space->user_regions_count);
        }

        add_to_regions_list(used, &space->user_used_regions);
        spinlock_release_preempt(&space->lock);
        return true;
    }

    spinlock_release_preempt(&space->lock);
    return false;
}
