#include <pmm.h>

pmm_region_t pmm_regions[PMM_MAX_REGIONS];
size_t       pmm_region_count = 0;
size_t       pmm_total_memory = 0;

static size_t bytes_to_kb(size_t bytes)
{
    return bytes / 1024;
}

static size_t bytes_to_mb(size_t bytes)
{
    return bytes / (1024 * 1024);
}

void pmm_init(struct boot_info* boot_info)
{
    struct boot_memmap_entry* memmap             = (struct boot_memmap_entry*) (uintptr_t) boot_info->memmap_entries;
    size_t                    memmap_entry_count = boot_info->memmap_entry_count;

    for (size_t i = 0; i < memmap_entry_count; i++)
    {
        if (memmap[i].type == BOOT_MEMMAP_USABLE)
        {
            if (pmm_region_count < PMM_MAX_REGIONS)
            {
                pmm_regions[pmm_region_count].base = align_up(memmap[i].base, PAGE_SIZE);
                pmm_regions[pmm_region_count].end  = align_down(memmap[i].base + memmap[i].length, PAGE_SIZE);
                pmm_total_memory += pmm_regions[pmm_region_count].end - pmm_regions[pmm_region_count].base;
                pmm_region_count++;
            }
            else
            {
                kprintf("Arx kernel: too many usable memory regions (max %u)\n", PMM_MAX_REGIONS);
                panic();
            }
        }
    }

    kprintf("Arx kernel: total usable memory: (%llu bytes, %llu KB, %llu MB)\n", (unsigned long long) pmm_total_memory, (unsigned long long) bytes_to_kb(pmm_total_memory), (unsigned long long) bytes_to_mb(pmm_total_memory));
}