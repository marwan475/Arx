#include <memory/vmm.h>

virt_addr_t KERNEL_VIRTUAL_BASE;
virt_addr_t KERNEL_VIRTUAL_END;

virt_addr_t USER_VIRTUAL_BASE;
virt_addr_t USER_VIRTUAL_END;

size_t MAX_REGIONS;

static virt_addr_space_t init_kernel_address_space = {0};

static virt_region_t* alloc_region_struct(void* metadata, size_t* metadata_count)
{
    for (size_t i = 0; i < MAX_REGIONS; i++)
    {
        virt_region_t* region = &((virt_region_t*) metadata)[i];
        if (!region->allocated)
        {
            (*metadata_count)++;
            region->allocated = true;
            return region;
        }
    }
    return NULL;
}

static void free_region_struct(virt_region_t* region, size_t* metadata_count)
{
    if (region == NULL || !region->allocated)
    {
        return;
    }
    region->allocated = false;
    (*metadata_count)--;
}

static void add_to_regions_list(virt_region_t* new_region, virt_region_t** list_head)
{
    new_region->next = *list_head;
    if (*list_head != NULL)
    {
        (*list_head)->prev = new_region;
    }
    *list_head = new_region;
}

static void remove_from_regions_list(virt_region_t* region, virt_region_t** list_head)
{
    if (region->prev != NULL)
    {
        region->prev->next = region->next;
    }
    else
    {
        *list_head = region->next;
    }
    if (region->next != NULL)
    {
        region->next->prev = region->prev;
    }
    region->next = NULL;
    region->prev = NULL;
}

static virt_region_t* handle_full_cover_case(virt_region_t* free, size_t* metadata_count, bool* stop)
{
    virt_region_t* next_free = free->next;
    *stop                    = false;

    if (free->prev == NULL)
    {
        if (next_free != NULL)
        {
            if (next_free->next != NULL)
            {
                next_free->next->prev = free;
            }
            *free      = *next_free;
            free->prev = NULL;
            free_region_struct(next_free, metadata_count);
            return free;
        }

        free->start = 0;
        free->end   = 0;
        free->size  = 0;
        free->next  = NULL;
        free->prev  = NULL;
        *stop       = true;
        return NULL;
    }

    free->prev->next = free->next;
    if (free->next != NULL)
    {
        free->next->prev = free->prev;
    }
    free_region_struct(free, metadata_count);
    return next_free;
}

static virt_region_t* handle_left_trim_case(virt_region_t* free, virt_addr_t overlap_end)
{
    free->start = overlap_end;
    free->size  = free->end - free->start;
    return free->next;
}

static virt_region_t* handle_right_trim_case(virt_region_t* free, virt_addr_t overlap_start)
{
    free->end  = overlap_start;
    free->size = free->end - free->start;
    return free->next;
}

static virt_region_t* handle_middle_split_case(virt_region_t* free, virt_addr_t overlap_start, virt_addr_t overlap_end, void* metadata, size_t* metadata_count)
{
    virt_addr_t old_end = free->end;
    free->end           = overlap_start;
    free->size          = free->end - free->start;

    virt_region_t* right_region = alloc_region_struct(metadata, metadata_count);
    if (right_region == NULL)
    {
        panic();
    }

    right_region->start = overlap_end;
    right_region->end   = old_end;
    right_region->size  = right_region->end - right_region->start;
    right_region->type  = free->type;
    right_region->next  = free->next;
    right_region->prev  = free;
    if (free->next != NULL)
    {
        free->next->prev = right_region;
    }
    free->next = right_region;

    return right_region->next;
}

static void remove_existing_mappings(virt_region_t* free_regions, virt_region_t* used_regions, void* metadata, size_t* metadata_count)
{
    if (free_regions == NULL || used_regions == NULL || metadata == NULL || metadata_count == NULL)
    {
        return;
    }

    for (virt_region_t* used = used_regions; used != NULL; used = used->next)
    {
        virt_region_t* free = free_regions;

        while (free != NULL)
        {
            virt_region_t* next_free = free->next;

            virt_addr_t overlap_start = free->start > used->start ? free->start : used->start;
            virt_addr_t overlap_end   = free->end < used->end ? free->end : used->end;

            if (overlap_start >= overlap_end)
            {
                free = next_free;
                continue;
            }

            bool touches_left  = overlap_start == free->start;
            bool touches_right = overlap_end == free->end;

            if (touches_left && touches_right)
            {
                bool stop = false;
                free      = handle_full_cover_case(free, metadata_count, &stop);
                if (stop)
                {
                    break;
                }
                continue;
            }

            if (touches_left)
            {
                free = handle_left_trim_case(free, overlap_end);
                continue;
            }

            if (touches_right)
            {
                free = handle_right_trim_case(free, overlap_start);
                continue;
            }

            free = handle_middle_split_case(free, overlap_start, overlap_end, metadata, metadata_count);
        }
    }
}

void vmm_init(struct boot_info* boot_info)
{
    (void) boot_info;

    KERNEL_VIRTUAL_BASE = CANONICAL_KERNEL_BASE;
    KERNEL_VIRTUAL_END  = CANONICAL_KERNEL_END;

    USER_VIRTUAL_BASE = CANONICAL_USER_BASE;
    USER_VIRTUAL_END  = CANONICAL_USER_END;

    MAX_REGIONS = REGION_METADATA_SIZE / sizeof(virt_region_t);

    init_kernel_address_space.type = VIRT_ADDR_KERNEL;
    init_kernel_address_space.pt   = arch_get_pt();
    init_kernel_address_space.lock = 0;

    init_kernel_address_space.kernel_free_regions    = NULL;
    init_kernel_address_space.kernel_used_regions    = NULL;
    init_kernel_address_space.kernel_region_metadata = pmm_alloc(REGION_METADATA_SIZE);
    memset(init_kernel_address_space.kernel_region_metadata, 0, REGION_METADATA_SIZE);
    init_kernel_address_space.kernel_regions_count = 0;

    init_kernel_address_space.user_free_regions    = NULL;
    init_kernel_address_space.user_used_regions    = NULL;
    init_kernel_address_space.user_region_metadata = pmm_alloc(REGION_METADATA_SIZE);
    memset(init_kernel_address_space.user_region_metadata, 0, REGION_METADATA_SIZE);
    init_kernel_address_space.user_regions_count = 0;

    // initial regions before removing existing mappings
    virt_region_t* initial_kernel_region          = alloc_region_struct(init_kernel_address_space.kernel_region_metadata, &init_kernel_address_space.kernel_regions_count);
    initial_kernel_region->start                  = KERNEL_VIRTUAL_BASE;
    initial_kernel_region->end                    = KERNEL_VIRTUAL_END;
    initial_kernel_region->size                   = KERNEL_VIRTUAL_END - KERNEL_VIRTUAL_BASE;
    initial_kernel_region->type                   = VIRT_ADDR_KERNEL;
    init_kernel_address_space.kernel_free_regions = initial_kernel_region;

    // existing regions
    virt_region_t* existing_kernel_region         = alloc_region_struct(init_kernel_address_space.kernel_region_metadata, &init_kernel_address_space.kernel_regions_count);
    existing_kernel_region->start                 = boot_info->kernel_start;
    existing_kernel_region->end                   = boot_info->kernel_end;
    existing_kernel_region->size                  = boot_info->kernel_end - boot_info->kernel_start;
    existing_kernel_region->type                  = VIRT_ADDR_KERNEL;
    init_kernel_address_space.kernel_used_regions = existing_kernel_region;

    virt_region_t* existing_hhdm_region = alloc_region_struct(init_kernel_address_space.kernel_region_metadata, &init_kernel_address_space.kernel_regions_count);
    existing_hhdm_region->start         = boot_info->hhdm_present ? boot_info->hhdm_start : 0;
    existing_hhdm_region->end           = boot_info->hhdm_present ? boot_info->hhdm_end : 0;
    existing_hhdm_region->size          = boot_info->hhdm_present ? (boot_info->hhdm_end - boot_info->hhdm_start) : 0;
    existing_hhdm_region->type          = VIRT_ADDR_KERNEL;
    if (boot_info->hhdm_present)
    {
        add_to_regions_list(existing_hhdm_region, &init_kernel_address_space.kernel_used_regions);
    }

    virt_region_t* existing_framebuffer_region = alloc_region_struct(init_kernel_address_space.kernel_region_metadata, &init_kernel_address_space.kernel_regions_count);
    existing_framebuffer_region->start         = boot_info->framebuffer_addr;
    existing_framebuffer_region->end           = boot_info->framebuffer_addr + boot_info->framebuffer_width * boot_info->framebuffer_height * (boot_info->framebuffer_bpp / 8);
    existing_framebuffer_region->size          = boot_info->framebuffer_width * boot_info->framebuffer_height * (boot_info->framebuffer_bpp / 8);
    existing_framebuffer_region->type          = VIRT_ADDR_KERNEL;
    add_to_regions_list(existing_framebuffer_region, &init_kernel_address_space.kernel_used_regions);

    remove_existing_mappings(init_kernel_address_space.kernel_free_regions, init_kernel_address_space.kernel_used_regions, init_kernel_address_space.kernel_region_metadata, &init_kernel_address_space.kernel_regions_count);

    virt_region_t* initial_user_region          = alloc_region_struct(init_kernel_address_space.user_region_metadata, &init_kernel_address_space.user_regions_count);
    initial_user_region->start                  = USER_VIRTUAL_BASE;
    initial_user_region->end                    = USER_VIRTUAL_END;
    initial_user_region->size                   = USER_VIRTUAL_END - USER_VIRTUAL_BASE;
    initial_user_region->type                   = VIRT_ADDR_USER;
    init_kernel_address_space.user_free_regions = initial_user_region;

    kprintf("Arx kernel: virtual memory manager initialized\n");

    uint8_t num_cpus = boot_info->smp.cpu_count < BOOT_SMP_MAX_CPUS ? boot_info->smp.cpu_count : BOOT_SMP_MAX_CPUS;
    for (uint8_t i = 0; i < num_cpus; i++)
    {
        dispatcher.cpus[i].address_space = &init_kernel_address_space;
    }
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

virt_addr_t vmm_reserve_region(virt_addr_space_t* space, size_t size, virt_type_t type)
{
    if (space == NULL || size == 0)
    {
        return 0;
    }

    size_t aligned_size = align_up(size, PAGE_SIZE);
    if (aligned_size == 0)
    {
        return 0;
    }

    virt_region_t** free_regions   = NULL;
    virt_region_t** used_regions   = NULL;
    void*           metadata       = NULL;
    size_t*         metadata_count = NULL;

    if (type == VIRT_ADDR_KERNEL)
    {
        free_regions   = &space->kernel_free_regions;
        used_regions   = &space->kernel_used_regions;
        metadata       = space->kernel_region_metadata;
        metadata_count = &space->kernel_regions_count;
    }
    else
    {
        free_regions   = &space->user_free_regions;
        used_regions   = &space->user_used_regions;
        metadata       = space->user_region_metadata;
        metadata_count = &space->user_regions_count;
    }

    spinlock_acquire(&space->lock);

    for (virt_region_t* free = *free_regions; free != NULL; free = free->next)
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
            spinlock_release(&space->lock);
            return reserved_start;
        }

        virt_region_t* used = alloc_region_struct(metadata, metadata_count);
        if (used == NULL)
        {
            spinlock_release(&space->lock);
            return 0;
        }

        used->start = reserved_start;
        used->end   = reserved_start + aligned_size;
        used->size  = aligned_size;
        used->type  = type;
        used->next  = NULL;
        used->prev  = NULL;

        free->start += aligned_size;
        free->size = free->end - free->start;

        add_to_regions_list(used, used_regions);
        spinlock_release(&space->lock);
        return reserved_start;
    }

    spinlock_release(&space->lock);
    return 0;
}

void vmm_free_region(virt_addr_space_t* space, virt_addr_t addr)
{
    if (space == NULL || addr == 0)
    {
        return;
    }

    spinlock_acquire(&space->lock);

    virt_region_t*  region         = NULL;
    virt_region_t** used_regions   = NULL;
    virt_region_t** free_regions   = NULL;
    size_t*         metadata_count = NULL;

    for (virt_region_t* it = space->kernel_used_regions; it != NULL; it = it->next)
    {
        if (it->start == addr)
        {
            region         = it;
            used_regions   = &space->kernel_used_regions;
            free_regions   = &space->kernel_free_regions;
            metadata_count = &space->kernel_regions_count;
            break;
        }
    }

    if (region == NULL)
    {
        for (virt_region_t* it = space->user_used_regions; it != NULL; it = it->next)
        {
            if (it->start == addr)
            {
                region         = it;
                used_regions   = &space->user_used_regions;
                free_regions   = &space->user_free_regions;
                metadata_count = &space->user_regions_count;
                break;
            }
        }
    }

    if (region == NULL)
    {
        spinlock_release(&space->lock);
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
        if (*free_regions == NULL)
        {
            region->prev  = NULL;
            region->next  = NULL;
            *free_regions = region;
        }
        else
        {
            virt_region_t* tail = *free_regions;
            while (tail->next != NULL)
            {
                tail = tail->next;
            }
            tail->next   = region;
            region->prev = tail;
            region->next = NULL;
        }
    }
    else
    {
        region->next = insert_before;
        region->prev = insert_before->prev;
        if (insert_before->prev != NULL)
        {
            insert_before->prev->next = region;
        }
        else
        {
            *free_regions = region;
        }
        insert_before->prev = region;
    }

    if (region->prev != NULL && region->prev->end == region->start)
    {
        virt_region_t* left = region->prev;
        left->end           = region->end;
        left->size          = left->end - left->start;
        left->next          = region->next;
        if (region->next != NULL)
        {
            region->next->prev = left;
        }
        free_region_struct(region, metadata_count);
        region = left;
    }

    if (region->next != NULL && region->end == region->next->start)
    {
        virt_region_t* right = region->next;
        region->end          = right->end;
        region->size         = region->end - region->start;
        region->next         = right->next;
        if (right->next != NULL)
        {
            right->next->prev = region;
        }
        free_region_struct(right, metadata_count);
    }

    spinlock_release(&space->lock);
}

virt_region_t* vmm_find_region(virt_addr_space_t* space, virt_addr_t addr)
{
    if (space == NULL || addr == 0)
    {
        return NULL;
    }

    spinlock_acquire(&space->lock);

    for (virt_region_t* it = space->kernel_used_regions; it != NULL; it = it->next)
    {
        if (it->start <= addr && addr < it->end)
        {
            spinlock_release(&space->lock);
            return it;
        }
    }

    for (virt_region_t* it = space->user_used_regions; it != NULL; it = it->next)
    {
        if (it->start <= addr && addr < it->end)
        {
            spinlock_release(&space->lock);
            return it;
        }
    }

    spinlock_release(&space->lock);
    return NULL;
}
