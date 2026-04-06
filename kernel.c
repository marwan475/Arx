#include <boot/boot.h>
#include <klib/klib.h>
#include <pmm.h>
#include <stdint.h>

// From bootloader we need
// - memory map
// - framebuffer info
// - smp info
// - higher half direct map instead of identity mapping so user address space is separate from physical memory addresses
// - paging with no user access and RWX on direct map
void kmain(struct boot_info* boot_info, uint64_t cpu_count)
{
    kprintf("Arx kernel: kmain entered\n");

    if (boot_info == 0 || boot_info->limine_present == 0)
    {
        kprintf("Arx kernel: no boot protocol info\n");
        panic();
    }

    if (boot_info->memmap_entries == 0 || boot_info->memmap_entry_count == 0)
    {
        kprintf("Arx kernel: no memory map entries\n");
        panic();
    }

    if (boot_info->memmap_entry_count > BOOT_MEMMAP_MAX_ENTRIES)
    {
        kprintf("Arx kernel: too many memory map entries (max %u)\n", BOOT_MEMMAP_MAX_ENTRIES);
        panic();
    }

    if (boot_info->framebuffer_addr == 0)
    {
        kprintf("Arx kernel: no framebuffer\n");
        panic();
    }

    kprintf("Arx kernel: framebuffer addr=0x%llx size=%llu x %llu pitch=%llu bpp=%llu\n", (unsigned long long) boot_info->framebuffer_addr, (unsigned long long) boot_info->framebuffer_width,
            (unsigned long long) boot_info->framebuffer_height, (unsigned long long) boot_info->framebuffer_pitch, (unsigned long long) boot_info->framebuffer_bpp);

    arch_smp_init(boot_info);

    pmm_init(boot_info);

    for (;;)
    {
    }
}
