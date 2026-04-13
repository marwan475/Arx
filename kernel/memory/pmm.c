/*
 * pmm.c
 *
 * Physical Memory Manager (PMM) implementation
 *
 * - Initializes memory regions based on bootloader information
 * - Sets up memory zones
 * - Seeds per-zone buddy allocator free lists and page metadata
 * - Handles buddy allocation and freeing of physical pages
 * - Implements thread safe PMM API
 *
 * Author: Marwan Mostafa
 *
 */

#include <memory/pmm.h>
#include <memory/vmm.h>

// keep it to one zone containing all memory for now
static numa_node_t pmm_numa_node = {0};

static size_t bytes_to_kb(size_t bytes)
{
    return bytes / 1024;
}

static size_t bytes_to_mb(size_t bytes)
{
    return bytes / (1024 * 1024);
}

static inline uint64_t pa_to_pfn(phys_addr_t pa)
{
    return pa >> PAGE_SHIFT;
}

static inline phys_addr_t pfn_to_pa(uint64_t pfn)
{
    return pfn << PAGE_SHIFT;
}

// this function should only be used before main pmm is setup
uint64_t pmm_alloc_from_region(zone_t* zone, size_t page_count)
{
    for (size_t i = 0; i < zone->region_count; i++)
    {
        if (zone->regions[i].page_count >= page_count)
        {
            uint64_t allocated_pfn = zone->regions[i].start_pfn;
            zone->regions[i].start_pfn += page_count;
            zone->regions[i].page_count -= page_count;
            return allocated_pfn;
        }
    }
    return PMM_INVALID_PFN;
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

void add_block_to_free_list(zone_t* zone, size_t order, uint64_t pfn)
{
    page_t* block = &zone->buddy_metadata[pfn];
    block->flags  = PMM_PAGE_FREE;
    block->order  = order;
    block->next   = NULL;
    block->prev   = NULL;

    free_list_t* free_list = &zone->buddy_free_lists[order];
    if (free_list->head == NULL)
    {
        free_list->head = block;
    }
    else
    {
        block->next           = free_list->head;
        free_list->head->prev = block;
        free_list->head       = block;
    }
}

void buddy_seed_region(zone_t* zone, size_t region_index)
{
    uint64_t start_pfn       = zone->regions[region_index].start_pfn;
    uint64_t end_pfn         = zone->regions[region_index].end_pfn;
    size_t   remaining_pages = zone->regions[region_index].page_count;

    // For an order to be valid its block size must be less than or equal to remaining pages
    // and the start pfn must be aligned to the block size
    while (remaining_pages > 0)
    {
        size_t order = MAX_ORDER;

        // on while loop exit the order is valid
        // goal is to find largest valid order
        while (order > 0)
        {
            if (order_size(order) <= remaining_pages && is_aligned_to_order(start_pfn, order))
            {
                break;
            }
            order--;
        }

        add_block_to_free_list(zone, order, start_pfn);
        start_pfn += order_size(order);
        remaining_pages -= order_size(order);
    }
}

// since we memset buddy_metadata to 0 all pages are set to reserved with order 0
// we only need to set usable pages to free with their pfns then seed the buddy free lists
void buddy_allocator_init(zone_t* zone)
{
    for (size_t i = 0; i < zone->region_count; i++)
    {
        uint64_t start_pfn = zone->regions[i].start_pfn;
        uint64_t end_pfn   = zone->regions[i].end_pfn;

        for (uint64_t pfn = start_pfn; pfn < end_pfn; pfn++)
        {
            page_t* page = &zone->buddy_metadata[pfn];
            page->pfn    = pfn;
            page->flags  = PMM_PAGE_FREE;
            page->order  = 0;
            page->next   = NULL;
            page->prev   = NULL;
        }

        buddy_seed_region(zone, i);
    }
}

void pmm_init(struct boot_info* boot_info)
{
    memset(&pmm_numa_node, 0, sizeof(pmm_numa_node));
    zone_t* zone = &pmm_numa_node.zone;

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

            if (zone->region_count < PMM_MAX_REGIONS)
            {
                zone->total_memory += aligned_end - aligned_base;

                // Store usable page frame numbers for buddy allocator
                const uint64_t start_pfn                     = pa_to_pfn(aligned_base);
                const uint64_t end_pfn                       = pa_to_pfn(aligned_end);
                zone->regions[zone->region_count].start_pfn  = start_pfn;
                zone->regions[zone->region_count].end_pfn    = end_pfn;
                zone->regions[zone->region_count].page_count = end_pfn - start_pfn;
                zone->total_pages += end_pfn - start_pfn;
                zone->region_count++;
                if (zone->min_pfn == 0 || start_pfn < zone->min_pfn)
                {
                    zone->min_pfn = start_pfn;
                }
                if (end_pfn > zone->max_pfn)
                {
                    zone->max_pfn = end_pfn;
                }
            }
            else
            {
                kprintf("Arx kernel: too many usable memory regions (max %u)\n", PMM_MAX_REGIONS);
                panic();
            }
        }
    }

    kprintf("Arx kernel: total usable memory: %llu bytes (%llu KB, %llu MB)\n", (unsigned long long) zone->total_memory, (unsigned long long) bytes_to_kb(zone->total_memory),
            (unsigned long long) bytes_to_mb(zone->total_memory));
    kprintf("Arx kernel: total usable Page Frames: %llu (min: %llu, max: %llu)\n", (unsigned long long) zone->total_pages, (unsigned long long) zone->min_pfn, (unsigned long long) zone->max_pfn);

    size_t buddy_metadata_size  = align_up((zone->max_pfn * sizeof(page_t)), PAGE_SIZE);
    size_t buddy_metadata_pages = buddy_metadata_size / PAGE_SIZE;

    uint64_t buddy_metadata_pfn = pmm_alloc_from_region(zone, buddy_metadata_pages);
    if (buddy_metadata_pfn == PMM_INVALID_PFN)
    {
        kprintf("Arx kernel: failed to allocate memory for buddy allocator metadata\n");
        panic();
    }

    zone->hhdm_present = boot_info->hhdm_present;
    zone->hhdm_offset  = boot_info->hhdm_offset;

    virt_addr_t buddy_metadata_va = pa_to_hhdm(pfn_to_pa(buddy_metadata_pfn), zone->hhdm_present, zone->hhdm_offset);
    memset((void*) buddy_metadata_va, 0, buddy_metadata_size);
    zone->buddy_metadata = (page_t*) buddy_metadata_va;

    zone->free_pages = zone->total_pages;
    zone->used_pages = 0;
    zone->lock       = 0;

    buddy_allocator_init(zone);
    kprintf("Arx kernel: buddy allocator initialized\n");

    dispatcher.cpus[arch_cpu_id()].numa_node = &pmm_numa_node;
}

// removes head of free list
page_t* remove_block_from_free_list(zone_t* zone, size_t order)
{
    free_list_t* free_list = &zone->buddy_free_lists[order];
    if (free_list->head == NULL)
    {
        return NULL;
    }

    page_t* block   = free_list->head;
    free_list->head = block->next;
    if (free_list->head != NULL)
    {
        free_list->head->prev = NULL;
    }
    block->next  = NULL;
    block->prev  = NULL;
    block->flags = PMM_PAGE_USED;

    return block;
}

page_t* split_block(zone_t* zone, page_t* block, size_t from_order, size_t to_order)
{
    if (from_order <= to_order)
    {
        return block;
    }

    while (from_order > to_order)
    {
        from_order--;
        uint64_t buddy_pfn = find_buddy_pfn(block->pfn, from_order); // new buddy pfn should be in block so no need to check if its valid
        add_block_to_free_list(zone, from_order, buddy_pfn);
        block->order = from_order;
    }

    return block;
}

uint64_t buddy_alloc(zone_t* zone, size_t order)
{
    size_t original_order = order;
    // find first order with free blocks that can satisfy request
    while (order <= MAX_ORDER && zone->buddy_free_lists[order].head == NULL)
    {
        order++;
    }

    if (order > MAX_ORDER)
    {
        return PMM_INVALID_PFN;
    }

    page_t* block = remove_block_from_free_list(zone, order);
    if (block == NULL)
    {
        return PMM_INVALID_PFN;
    }

    if (order == original_order)
    {
        return block->pfn;
    }

    // if higher order then requested split block
    block        = split_block(zone, block, order, original_order);
    block->flags = PMM_PAGE_USED;
    return block->pfn;
}

// merges block with buddy, only does one merge
page_t* merge_block(zone_t* zone, page_t* block)
{
    if (block->order >= MAX_ORDER)
    {
        return NULL;
    }

    uint64_t buddy_pfn = find_buddy_pfn(block->pfn, block->order);
    page_t*  buddy     = &zone->buddy_metadata[buddy_pfn];

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
        zone->buddy_free_lists[buddy->order].head = buddy->next;
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

void buddy_free(zone_t* zone, uint64_t pfn)
{
    page_t* block = &zone->buddy_metadata[pfn];
    size_t  order = block->order;

    while (order < MAX_ORDER)
    {
        page_t* merged = merge_block(zone, block);
        if (merged == NULL)
        {
            break;
        }
        block = merged;
        order = block->order;
    }

    add_block_to_free_list(zone, block->order, block->pfn);
}

void* pmm_alloc(zone_t* zone, size_t size)
{
    if (zone == NULL)
    {
        return NULL;
    }

    if (size == 0)
    {
        return NULL;
    }

    size_t order = 0;
    while (order <= MAX_ORDER && order_size(order) * PAGE_SIZE < size)
    {
        order++;
    }

    if (order > MAX_ORDER)
    {
        return NULL;
    }

    spinlock_acquire(&zone->lock);

    uint64_t pfn = buddy_alloc(zone, order);

    if (pfn == PMM_INVALID_PFN)
    {
        spinlock_release(&zone->lock);
        return NULL;
    }

    zone->free_pages -= order_size(order);
    zone->used_pages += order_size(order);

    spinlock_release(&zone->lock);
    virt_addr_t hhdm_va = pa_to_hhdm(pfn_to_pa(pfn), zone->hhdm_present, zone->hhdm_offset);
    return (void*) hhdm_va;
}

void pmm_free(zone_t* zone, void* addr)
{
    if (zone == NULL)
    {
        return;
    }

    if (addr == NULL)
    {
        return;
    }

    virt_addr_t hhdm_va = (virt_addr_t) (uintptr_t) addr;
    phys_addr_t pa      = hhdm_to_pa(hhdm_va, zone->hhdm_present, zone->hhdm_offset);
    uint64_t    pfn     = pa_to_pfn(pa);

    spinlock_acquire(&zone->lock);

    size_t allocated_order = zone->buddy_metadata[pfn].order;

    buddy_free(zone, pfn);

    zone->free_pages += order_size(allocated_order);
    zone->used_pages -= order_size(allocated_order);

    spinlock_release(&zone->lock);
}
