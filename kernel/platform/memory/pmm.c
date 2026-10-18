#include <klib/intrusive_list.h>
#include <memory/pmm.h>
#include <memory/numa.h>
#include <memory/vmm.h>
#include <platform.h>

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
static uint64_t pmm_alloc_from_region(zone_t* zone, size_t page_count)
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

static inline uint64_t order_size(size_t order)
{
    return 1UL << order;
}

static inline bool is_aligned_to_order(uint64_t pfn, size_t order)
{
    return (pfn & (order_size(order) - 1)) == 0;
}

// to find buddy pfn base pfn needs to be aligned to order for this calculation to work
static inline uint64_t find_buddy_pfn(uint64_t pfn, size_t order)
{
    return pfn ^ order_size(order);
}

static void buddy_allocator_init(zone_t* zone);

static bool zone_contains(const zone_t* zone, uint64_t pfn, size_t pages)
{
    for (size_t i = 0; i < zone->region_count; i++)
    {
        if (pfn >= zone->regions[i].start_pfn && pfn < zone->regions[i].end_pfn && pages <= zone->regions[i].end_pfn - pfn)
        {
            return true;
        }
    }

    return false;
}

static void split_dma_zone(numa_node_t* node)
{
    zone_t*  normal   = &node->zone;
    zone_t*  dma      = &node->dma_zone;
    size_t   lowPages = 0;
    uint64_t limit    = UINT64_C(0x100000000) / PAGE_SIZE;

    for (size_t i = 0; i < normal->region_count; i++)
    {
        uint64_t end = normal->regions[i].end_pfn < limit ? normal->regions[i].end_pfn : limit;

        if (end > normal->regions[i].start_pfn)
        {
            lowPages += end - normal->regions[i].start_pfn;
        }
    }

    size_t pages = lowPages / 16;
    size_t cap   = (64UL * 1024 * 1024) / PAGE_SIZE;
    pages        = pages < cap ? pages : cap;
    pages        = (pages / (1UL << MAX_ORDER)) * (1UL << MAX_ORDER);

    dma->hhdm_present   = normal->hhdm_present;
    dma->hhdm_offset    = normal->hhdm_offset;
    dma->buddy_metadata = normal->buddy_metadata;

    // Both zones share the node's PFN metadata, but own disjoint regions/lists.
    for (size_t i = 0; i < normal->region_count && pages != 0; i++)
    {
        pmm_region_t* region = &normal->regions[i];
        uint64_t      end    = region->end_pfn < limit ? region->end_pfn : limit;

        if (end <= region->start_pfn)
        {
            continue;
        }

        size_t count           = end - region->start_pfn;
        count                  = count < pages ? count : pages;
        pmm_region_t* reserved = &dma->regions[dma->region_count++];
        reserved->start_pfn    = region->start_pfn;
        reserved->end_pfn      = region->start_pfn + count;
        reserved->page_count   = count;
        if (dma->region_count == 1 || reserved->start_pfn < dma->min_pfn)
        {
            dma->min_pfn = reserved->start_pfn;
        }

        if (reserved->end_pfn > dma->max_pfn)
        {
            dma->max_pfn = reserved->end_pfn;
        }

        dma->total_pages += count;
        region->start_pfn += count;
        region->page_count -= count;
        normal->total_pages -= count;
        pages -= count;
    }

    dma->free_pages      = dma->total_pages;
    dma->total_memory    = dma->total_pages * PAGE_SIZE;
    normal->free_pages   = normal->total_pages;
    normal->total_memory = normal->total_pages * PAGE_SIZE;
    buddy_allocator_init(dma);
    kprintf("Arx kernel: DMA zone reserved %llu bytes below 4 GiB\n", (unsigned long long) dma->total_memory);
}

static void add_block_to_free_list(zone_t* zone, size_t order, uint64_t pfn)
{
    page_t* block = &zone->buddy_metadata[pfn];
    block->flags  = PMM_PAGE_FREE;
    block->order  = order;
    ILIST_NODE_INIT(block);

    free_list_t* free_list = &zone->buddy_free_lists[order];
    ILIST_PUSH_FRONT(free_list->head, block);
}

static void buddy_seed_region(zone_t* zone, size_t region_index)
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
static void buddy_allocator_init(zone_t* zone)
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

static void add_usable_region(zone_t* zone, uint64_t start, uint64_t end)
{
    if (zone->region_count == PMM_MAX_REGIONS)
    {
        error_logger("Arx kernel: too many memory regions for NUMA zone\n");
        panic();
    }
    const uint64_t first = pa_to_pfn(start), last = pa_to_pfn(end);
    zone->regions[zone->region_count++] = (pmm_region_t) {first, last, last - first};
    zone->total_pages += last - first;
    zone->total_memory += end - start;
    if (zone->region_count == 1 || first < zone->min_pfn)
    {
        zone->min_pfn = first;
    }
    if (last > zone->max_pfn)
    {
        zone->max_pfn = last;
    }
}

static const numa_memory_range_t* memory_range(const numa_topology_t* topology, uint64_t address)
{
    for (size_t i = 0; i < topology->range_count; ++i)
    {
        if (address >= topology->ranges[i].start && address < topology->ranges[i].end)
        {
            return &topology->ranges[i];
        }
    }
    return NULL;
}

void pmm_init(struct boot_info* boot_info)
{
    numa_topology_t topology;
    bool topology_valid = numa_boot_topology(boot_info, &topology);
    const struct boot_memmap_entry* memmap = (const void*) boot_info->memmap_entries;

    // Reject incomplete firmware topology as a whole. Every usable byte must
    // have exactly one owner before any allocator state is published.
    for (size_t i = 0; topology_valid && i < boot_info->memmap_entry_count; ++i)
    {
        if (memmap[i].type != BOOT_MEMMAP_USABLE)
        {
            continue;
        }

        uint64_t start = align_up(memmap[i].base, PAGE_SIZE);
        const uint64_t end = align_down(memmap[i].base + memmap[i].length, PAGE_SIZE);

        while (start < end)
        {
            const numa_memory_range_t* range = memory_range(&topology, start);

            if (range == NULL)
            {
                topology_valid = false;
                break;
            }

            start = range->end < end ? range->end : end;
        }
    }

    memset(platform.numa_nodes, 0, sizeof(platform.numa_nodes));
    platform.numa_node_count = topology_valid ? topology.node_count : 1;
    size_t maximum_pfn = 0;

    for (size_t i = 0; i < boot_info->memmap_entry_count; ++i)
    {
        if (memmap[i].type != BOOT_MEMMAP_USABLE)
        {
            continue;
        }

        uint64_t start = align_up(memmap[i].base, PAGE_SIZE);
        const uint64_t end = align_down(memmap[i].base + memmap[i].length, PAGE_SIZE);

        while (start < end)
        {
            const numa_memory_range_t* range = topology_valid ? memory_range(&topology, start) : NULL;
            const uint64_t last = range != NULL && range->end < end ? range->end : end;
            zone_t* zone = &platform.numa_nodes[range != NULL ? range->node : 0].zone;
            add_usable_region(zone, start, last);

            if (zone->max_pfn > maximum_pfn)
            {
                maximum_pfn = zone->max_pfn;
            }

            start = last;
        }
    }

    const size_t metadata_size = align_up(maximum_pfn * sizeof(page_t), PAGE_SIZE);
    const size_t metadata_pages = metadata_size / PAGE_SIZE;
    uint64_t metadata_pfn = PMM_INVALID_PFN;

    for (size_t i = 0; i < platform.numa_node_count; ++i)
    {
        zone_t* zone = &platform.numa_nodes[i].zone;
        metadata_pfn = pmm_alloc_from_region(zone, metadata_pages);

        if (metadata_pfn != PMM_INVALID_PFN)
        {
            zone->total_pages -= metadata_pages;
            break;
        }
    }

    if (metadata_pfn == PMM_INVALID_PFN)
    {
        error_logger("Arx kernel: cannot allocate NUMA page metadata\n");
        panic();
    }

    page_t* metadata = (void*) (uintptr_t) pa_to_hhdm(pfn_to_pa(metadata_pfn), boot_info->hhdm_present, boot_info->hhdm_offset);
    memset(metadata, 0, metadata_size);

    for (size_t i = 0; i < platform.numa_node_count; ++i)
    {
        numa_node_t* node = &platform.numa_nodes[i];
        zone_t* zone = &node->zone;
        node->proximity_domain = topology_valid ? topology.domains[i] : 0;
        zone->hhdm_present = boot_info->hhdm_present;
        zone->hhdm_offset = boot_info->hhdm_offset;
        zone->buddy_metadata = metadata;
        split_dma_zone(node);
        buddy_allocator_init(zone);
        kprintf("Arx kernel: NUMA node %u domain %u Normal=%llu DMA=%llu bytes\n", (unsigned) i, node->proximity_domain, (unsigned long long) zone->total_memory, (unsigned long long) node->dma_zone.total_memory);
    }

    for (size_t i = 0; i < platform.cpu_count; ++i)
    {
        const size_t node = topology_valid ? topology.cpu_nodes[i] : 0;
        platform.cpus[i].numa_node = &platform.numa_nodes[node];
        ++platform.numa_nodes[node].cpu_count;
    }

    kprintf("Arx kernel: NUMA topology %s, %u nodes\n", topology_valid ? "validated" : "unavailable (uniform fallback)", (unsigned) platform.numa_node_count);
}

// removes head of free list
static page_t* remove_block_from_free_list(zone_t* zone, size_t order)
{
    free_list_t* free_list = &zone->buddy_free_lists[order];

    if (free_list->head == NULL)
    {
        return NULL;
    }

    page_t* block = free_list->head;
    ILIST_REMOVE(free_list->head, block);
    block->flags = PMM_PAGE_USED;

    return block;
}

static page_t* split_block(zone_t* zone, page_t* block, size_t from_order, size_t to_order)
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

static uint64_t buddy_alloc(zone_t* zone, size_t order)
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
static page_t* merge_block(zone_t* zone, page_t* block)
{
    if (block->order >= MAX_ORDER)
    {
        return NULL;
    }

    uint64_t buddy_pfn  = find_buddy_pfn(block->pfn, block->order);
    uint64_t merged_pfn = buddy_pfn < block->pfn ? buddy_pfn : block->pfn;

    if (!zone_contains(zone, buddy_pfn, order_size(block->order)) || !zone_contains(zone, merged_pfn, order_size(block->order + 1)))
    {
        return NULL;
    }

    page_t* buddy = &zone->buddy_metadata[buddy_pfn];

    if (buddy->flags != PMM_PAGE_FREE || buddy->order != block->order)
    {
        return NULL;
    }

    // remove buddy from free list
    ILIST_REMOVE(zone->buddy_free_lists[buddy->order].head, buddy);

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

static void buddy_free(zone_t* zone, uint64_t pfn)
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

static void* pmm_alloc_zone(zone_t* zone, size_t size)
{
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

    spinlock_acquire_preempt(&zone->lock);

    uint64_t pfn = buddy_alloc(zone, order);

    if (pfn == PMM_INVALID_PFN)
    {
        spinlock_release_preempt(&zone->lock);
        return NULL;
    }

    zone->free_pages -= order_size(order);
    zone->used_pages += order_size(order);

    spinlock_release_preempt(&zone->lock);
    virt_addr_t hhdm_va = pa_to_hhdm(pfn_to_pa(pfn), zone->hhdm_present, zone->hhdm_offset);
    return (void*) hhdm_va;
}

void* pmm_alloc(size_t size)
{
    numa_node_t* local = platform.cpus[arch_cpu_id()].numa_node;
    if (local == NULL)
    {
        return NULL;
    }
    void* pointer = pmm_alloc_zone(&local->zone, size);
    for (size_t i = 0; pointer == NULL && i < platform.numa_node_count; ++i)
    {
        if (&platform.numa_nodes[i] != local)
        {
            pointer = pmm_alloc_zone(&platform.numa_nodes[i].zone, size);
        }
    }
    return pointer;
}

static void* pmm_alloc_dma_zone(zone_t* zone, size_t size, phys_addr_t maximum_address, phys_addr_t* physical_address)
{
    if (size == 0 || platform.cpus[arch_cpu_id()].numa_node == NULL)
    {
        return NULL;
    }

    size_t  order = 0;

    while (order <= MAX_ORDER && order_size(order) * PAGE_SIZE < size)
    {
        order++;
    }

    if (order > MAX_ORDER)
    {
        return NULL;
    }

    spinlock_acquire_preempt(&zone->lock);
    page_t*  selected  = NULL;
    size_t   fromOrder = order;
    uint64_t bytes     = order_size(order) * PAGE_SIZE;

    for (; fromOrder <= MAX_ORDER && selected == NULL; fromOrder++)
    {
        for (page_t* block = zone->buddy_free_lists[fromOrder].head; block != NULL; block = block->next)
        {
            phys_addr_t address = pfn_to_pa(block->pfn);

            if (address <= maximum_address && bytes - 1 <= maximum_address - address)
            {
                selected = block;
                break;
            }
        }
    }

    if (selected == NULL)
    {
        spinlock_release_preempt(&zone->lock);
        return NULL;
    }

    fromOrder--;
    ILIST_REMOVE(zone->buddy_free_lists[fromOrder].head, selected);
    selected        = split_block(zone, selected, fromOrder, order);
    selected->flags = PMM_PAGE_USED;
    zone->free_pages -= order_size(order);
    zone->used_pages += order_size(order);
    phys_addr_t physical = pfn_to_pa(selected->pfn);

    if (physical_address != NULL)
    {
        *physical_address = physical;
    }

    spinlock_release_preempt(&zone->lock);
    return (void*) (uintptr_t) pa_to_hhdm(physical, zone->hhdm_present, zone->hhdm_offset);
}

void* pmm_alloc_dma(size_t size, phys_addr_t maximum_address, phys_addr_t* physical_address)
{
    numa_node_t* local = platform.cpus[arch_cpu_id()].numa_node;
    if (local == NULL)
    {
        return NULL;
    }
    void* pointer = pmm_alloc_dma_zone(&local->dma_zone, size, maximum_address, physical_address);
    for (size_t i = 0; pointer == NULL && i < platform.numa_node_count; ++i)
    {
        if (&platform.numa_nodes[i] != local)
        {
            pointer = pmm_alloc_dma_zone(&platform.numa_nodes[i].dma_zone, size, maximum_address, physical_address);
        }
    }
    return pointer;
}

void pmm_free(void* addr)
{
    if (addr == NULL)
    {
        return;
    }

    for (size_t node = 0; node < platform.numa_node_count; node++)
    {
        zone_t* zones[] = { &platform.numa_nodes[node].zone, &platform.numa_nodes[node].dma_zone };

        for (size_t i = 0; i < 2; i++)
        {
            zone_t*     zone = zones[i];
            phys_addr_t pa   = hhdm_to_pa((uintptr_t) addr, zone->hhdm_present, zone->hhdm_offset);
            uint64_t    pfn  = pa_to_pfn(pa);

            if ((pa & (PAGE_SIZE - 1)) != 0 || !zone_contains(zone, pfn, 1))
            {
                continue;
            }

            spinlock_acquire_preempt(&zone->lock);
            page_t* block = &zone->buddy_metadata[pfn];
            size_t  order = block->order;

            if (block->flags == PMM_PAGE_USED && order <= MAX_ORDER && is_aligned_to_order(pfn, order) && zone_contains(zone, pfn, order_size(order)))
            {
                buddy_free(zone, pfn);
                zone->free_pages += order_size(order);
                zone->used_pages -= order_size(order);
            }

            spinlock_release_preempt(&zone->lock);
            return;
        }
    }
}
