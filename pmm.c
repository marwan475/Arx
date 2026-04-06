#include <pmm.h>

size_t       pmm_total_memory        = 0;

pmm_region_t pmm_usable_regions[PMM_MAX_REGIONS];
size_t          pmm_usable_region_count = 0;
size_t          pmm_total_usable_pfns      = 0;

size_t min_pfn = 0;
size_t max_pfn = 0;

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
                const uint64_t start_pfn = pa_to_pfn(aligned_base);
                const uint64_t end_pfn   = pa_to_pfn(aligned_end);
                pmm_usable_regions[pmm_usable_region_count].start_pfn = start_pfn;
                pmm_usable_regions[pmm_usable_region_count].end_pfn     = end_pfn;
                pmm_usable_regions[pmm_usable_region_count].page_count  = end_pfn - start_pfn;
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

    size_t buddy_metadata_size = align_up((max_pfn * sizeof(page_t)), PAGE_SIZE);
    size_t buddy_metadata_pages = buddy_metadata_size / PAGE_SIZE;

    uint64_t buddy_metadata_pfn = pmm_alloc_from_region(buddy_metadata_pages);
    if (buddy_metadata_pfn == 0)
    {
        kprintf("Arx kernel: failed to allocate memory for buddy allocator metadata\n");
        panic();              
    }

    uintptr_t buddy_metadata_pa = pfn_to_pa(buddy_metadata_pfn);
    //memset((void*) buddy_metadata_pa, 0, buddy_metadata_size);
}