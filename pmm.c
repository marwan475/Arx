#include <pmm.h>

size_t pmm_total_memory = 0;

pmm_region_t pmm_usable_regions[PMM_MAX_REGIONS];
size_t       pmm_usable_region_count = 0;
size_t       pmm_total_usable_pfns   = 0;

size_t min_pfn = 0;
size_t max_pfn = 0;

page_t* buddy_metadata = NULL;
free_list_t buddy_free_lists[MAX_ORDER + 1]; 

static size_t bytes_to_kb(size_t bytes)
{
    return bytes / 1024;
}

static size_t bytes_to_mb(size_t bytes)
{
    return bytes / (1024 * 1024);
}

static inline uint64_t pa_to_pfn(uint64_t pa)
{
    return pa >> PAGE_SHIFT;
}

static inline uint64_t pfn_to_pa(uint64_t pfn)
{
    return pfn << PAGE_SHIFT;
}

// this function should only be used before main pmm is setup
uint64_t pmm_alloc_from_region(size_t page_count)
{
    for (size_t i = 0; i < pmm_usable_region_count; i++)
    {
        if (pmm_usable_regions[i].page_count >= page_count)
        {
            uint64_t allocated_pfn = pmm_usable_regions[i].start_pfn;
            pmm_usable_regions[i].start_pfn += page_count;
            pmm_usable_regions[i].page_count -= page_count;
            return allocated_pfn;
        }
    }
    return 0;
}

static _force_inline uint64_t order_size(size_t order)
{
    return 1UL << order;
}

static _force_inline bool is_aligned_to_order(uint64_t pfn, size_t order)
{
    return (pfn & (order_size(order) - 1)) == 0;
}

// to find buddy pfn base pfn needs to be aligned to order for this calculation to work
static _force_inline uint64_t find_buddy_pfn(uint64_t pfn, size_t order)
{
    return pfn ^ order_size(order);
}

void add_block_to_free_list(size_t order, uint64_t pfn)
{
    page_t* block = &buddy_metadata[pfn];
    block->flags = PMM_PAGE_FREE;
    block->order = order;
    block->next  = NULL;
    block->prev  = NULL;

    free_list_t* free_list = &buddy_free_lists[order];
    if (free_list->head == NULL)
    {
        free_list->head = block;
    }
    else
    {
        block->next = free_list->head;
        free_list->head->prev = block;
        free_list->head = block;
    }
}

void buddy_seed_region(size_t region_index)
{
    uint64_t start_pfn = pmm_usable_regions[region_index].start_pfn;
    uint64_t end_pfn   = pmm_usable_regions[region_index].end_pfn;
    size_t remaining_pages = pmm_usable_regions[region_index].page_count;

    // For an order to be valid its block size must be less then or equal to remaining pages
    // and the start pfn must be aligned to the block size
    while (remaining_pages > 0)
    {
        size_t order = MAX_ORDER;

        // on while loop exit the order is valid
        // goal is to find largest valid order
        while (order > 0){
            if (order_size(order) <= remaining_pages && is_aligned_to_order(start_pfn, order))
            {
                break;
            }
            order--;
        }

        add_block_to_free_list(order, start_pfn);
        start_pfn += order_size(order);
        remaining_pages -= order_size(order);
    }


}

// since we memset buddy_metadata to 0 all pages are set to reserved with order 0
// we only need to set usable pages to free with thier pfns then seed the buddy free lists
void buddy_allocator_init()
{
    for (size_t i = 0; i < pmm_usable_region_count; i++)
    {
        uint64_t start_pfn = pmm_usable_regions[i].start_pfn;
        uint64_t end_pfn   = pmm_usable_regions[i].end_pfn;

        for (uint64_t pfn = start_pfn; pfn < end_pfn; pfn++)
        {
            page_t* page = &buddy_metadata[pfn];
            page->pfn   = pfn;
            page->flags = PMM_PAGE_FREE;
            page->order = 0;
            page->next  = NULL;
            page->prev  = NULL;
        }

        buddy_seed_region(i);
    }
}

void pmm_init(struct boot_info* boot_info)
{
    struct boot_memmap_entry* memmap             = (struct boot_memmap_entry*) (uintptr_t) boot_info->memmap_entries;
    size_t                    memmap_entry_count = boot_info->memmap_entry_count;

    for (size_t i = 0; i < memmap_entry_count; i++)
    {
        if (memmap[i].type == BOOT_MEMMAP_USABLE)
        {
            uint64_t aligned_base = align_up(memmap[i].base, PAGE_SIZE);
            uint64_t aligned_end  = align_down(memmap[i].base + memmap[i].length, PAGE_SIZE);

            if (aligned_end <= aligned_base)
            {
                continue;
            }

            if (pmm_usable_region_count < PMM_MAX_REGIONS)
            {
                pmm_total_memory += aligned_end - aligned_base;

                // Store usable page frame numbers for buddy allocator
                const uint64_t start_pfn                               = pa_to_pfn(aligned_base);
                const uint64_t end_pfn                                 = pa_to_pfn(aligned_end);
                pmm_usable_regions[pmm_usable_region_count].start_pfn  = start_pfn;
                pmm_usable_regions[pmm_usable_region_count].end_pfn    = end_pfn;
                pmm_usable_regions[pmm_usable_region_count].page_count = end_pfn - start_pfn;
                pmm_total_usable_pfns += end_pfn - start_pfn;
                pmm_usable_region_count++;
                if (min_pfn == 0 || start_pfn < min_pfn)
                {
                    min_pfn = start_pfn;
                }
                if (end_pfn > max_pfn)
                {
                    max_pfn = end_pfn;
                }
            }
            else
            {
                kprintf("Arx kernel: too many usable memory regions (max %u)\n", PMM_MAX_REGIONS);
                panic();
            }
        }
    }

    kprintf("Arx kernel: total usable memory: %llu bytes (%llu KB, %llu MB)\n", (unsigned long long) pmm_total_memory, (unsigned long long) bytes_to_kb(pmm_total_memory),
            (unsigned long long) bytes_to_mb(pmm_total_memory));
    kprintf("Arx kernel: total usable Page Frames: %llu (min: %llu, max: %llu)\n", (unsigned long long) pmm_total_usable_pfns, (unsigned long long) min_pfn, (unsigned long long) max_pfn);

    size_t buddy_metadata_size  = align_up((max_pfn * sizeof(page_t)), PAGE_SIZE);
    size_t buddy_metadata_pages = buddy_metadata_size / PAGE_SIZE;

    uint64_t buddy_metadata_pfn = pmm_alloc_from_region(buddy_metadata_pages);
    if (buddy_metadata_pfn == 0)
    {
        kprintf("Arx kernel: failed to allocate memory for buddy allocator metadata\n");
        panic();
    }

    uintptr_t buddy_metadata_pa = pa_to_hhdm(pfn_to_pa(buddy_metadata_pfn), boot_info);
    memset((void*) buddy_metadata_pa, 0, buddy_metadata_size);
    buddy_metadata = (page_t*) buddy_metadata_pa;

    buddy_allocator_init();
    kprintf("Arx kernel: buddy allocator initialized\n");
}

// removes head of free list
page_t* remove_block_from_free_list(size_t order)
{
    free_list_t* free_list = &buddy_free_lists[order];
    if (free_list->head == NULL)
    {
        return NULL;
    }

    page_t* block = free_list->head;
    free_list->head = block->next;
    if (free_list->head != NULL)
    {
        free_list->head->prev = NULL;
    }
    block->next = NULL;
    block->prev = NULL;
    block->flags = PMM_PAGE_USED;

    return block;
}

page_t* split_block(page_t* block, size_t from_order, size_t to_order)
{
    if (from_order <= to_order)
    {
        return block;
    }

    while (from_order > to_order)
    {
        from_order--;
        uint64_t buddy_pfn = find_buddy_pfn(block->pfn, from_order); // new buddy pfn should be in block so no need to check if its valid
        add_block_to_free_list(from_order, buddy_pfn);
        block->order = from_order;
    }

    return block;
}

uint64_t buddy_alloc(size_t order)
{
    size_t original_order = order;
    // find first order with free blocks that can satisfy request
    while (order <= MAX_ORDER && buddy_free_lists[order].head == NULL)
    {
        order++;
    }

    if (order > MAX_ORDER)
    {
        return 0;
    }

    page_t* block = remove_block_from_free_list(order);
    if (block == NULL)
    {
        return 0;
    }

    if ( order == original_order)
    {
        return block->pfn;
    }

    // if higher order then requested split block
    block = split_block(block, order, original_order);
    block->flags = PMM_PAGE_USED;
    return block->pfn;
}

// merges block with buddy, only does one merge
page_t* merge_block(page_t* block)
{
    if (block->order >= MAX_ORDER)
    {
        return NULL;
    }

    uint64_t buddy_pfn = find_buddy_pfn(block->pfn, block->order);
    page_t*  buddy     = &buddy_metadata[buddy_pfn];

    if (buddy->flags != PMM_PAGE_FREE || buddy->order != block->order)
    {
        return NULL;
    }

    // remove buddy from free list
    if (buddy->prev != NULL)
    {
        buddy->prev->next = buddy->next;
    }
    else
    {
        buddy_free_lists[buddy->order].head = buddy->next;
    }
    if (buddy->next != NULL)
    {
        buddy->next->prev = buddy->prev;
    }

    // merge block and buddy
    if (buddy_pfn < block->pfn)
    {
        buddy->order++;
        return buddy;
    }
    else
    {
        block->order++;
        return block;
    }
}

void buddy_free(uint64_t pfn)
{
    page_t* block = &buddy_metadata[pfn];
    size_t order = block->order;

    while (order < MAX_ORDER)
    {
        block = merge_block(block);
        if (block == NULL)
        {
            break;
        }
        order = block->order;
    }

    add_block_to_free_list(block->order, block->pfn);

}