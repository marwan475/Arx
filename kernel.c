#include <boot/boot.h>
#include <klib/klib.h>
#include <stdint.h>

static void panic_halt(void)
{
    for (;;)
    {
    }
}

static const char* boot_memmap_type_to_string(uint64_t type)
{
    switch (type)
    {
        case BOOT_MEMMAP_USABLE:
            return "usable";
        case BOOT_MEMMAP_RESERVED:
            return "reserved";
        case BOOT_MEMMAP_ACPI_RECLAIMABLE:
            return "acpi_reclaimable";
        case BOOT_MEMMAP_ACPI_NVS:
            return "acpi_nvs";
        case BOOT_MEMMAP_BAD_MEMORY:
            return "bad_memory";
        case BOOT_MEMMAP_BOOTLOADER_RECLAIMABLE:
            return "bootloader_reclaimable";
        case BOOT_MEMMAP_KERNEL_AND_MODULES:
            return "kernel_and_modules";
        case BOOT_MEMMAP_FRAMEBUFFER:
            return "framebuffer";
        default:
            return "unknown";
    }
}

void kmain(struct boot_info* boot_info)
{
    struct boot_memmap_entry* memmap;

    kprintf("Arx kernel: kmain online\n");

    if (boot_info == 0 || boot_info->limine_present == 0)
    {
        kprintf("Arx kernel: no boot protocol info\n");
        panic_halt();
    }

    if (boot_info->memmap_entries == 0 || boot_info->memmap_entry_count == 0)
    {
        kprintf("Arx kernel: no Limine memory map entries\n");
        panic_halt();
    }

    kprintf("Arx kernel: memmap entries=%llu\n", (unsigned long long) boot_info->memmap_entry_count);

    memmap = (struct boot_memmap_entry*) (uintptr_t) boot_info->memmap_entries;
    for (uint64_t i = 0; i < boot_info->memmap_entry_count; i++)
    {
        kprintf("Arx kernel: memmap[%llu] base=0x%llx len=0x%llx type=%s\n",
                (unsigned long long) i,
                (unsigned long long) memmap[i].base,
                (unsigned long long) memmap[i].length,
                boot_memmap_type_to_string(memmap[i].type));
    }

    if (boot_info->framebuffer_addr == 0)
    {
        kprintf("Arx kernel: no Limine framebuffer response\n");
        panic_halt();
    }

    kprintf("Arx kernel: framebuffer addr=0x%llx size=%llu x %llu pitch=%llu bpp=%llu\n",
            (unsigned long long) boot_info->framebuffer_addr,
            (unsigned long long) boot_info->framebuffer_width,
            (unsigned long long) boot_info->framebuffer_height,
            (unsigned long long) boot_info->framebuffer_pitch,
            (unsigned long long) boot_info->framebuffer_bpp);

    for (;;)
    {
    }
}
