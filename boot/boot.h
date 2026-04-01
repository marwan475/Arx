#ifndef ARX_BOOT_H
#define ARX_BOOT_H

#include <stdint.h>

#define BOOT_MEMMAP_MAX_ENTRIES 256

enum boot_memmap_type
{
    BOOT_MEMMAP_USABLE                 = 0,
    BOOT_MEMMAP_RESERVED               = 1,
    BOOT_MEMMAP_ACPI_RECLAIMABLE       = 2,
    BOOT_MEMMAP_ACPI_NVS               = 3,
    BOOT_MEMMAP_BAD_MEMORY             = 4,
    BOOT_MEMMAP_BOOTLOADER_RECLAIMABLE = 5,
    BOOT_MEMMAP_KERNEL_AND_MODULES     = 6,
    BOOT_MEMMAP_FRAMEBUFFER            = 7,
};

struct boot_info
{
    uint64_t  limine_present;
    uint64_t  memmap_entry_count;
    uintptr_t memmap_entries;
    uint64_t  framebuffer_addr;
    uint64_t  framebuffer_width;
    uint64_t  framebuffer_height;
    uint64_t  framebuffer_pitch;
    uint64_t  framebuffer_bpp;
};

struct boot_memmap_entry
{
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

void kmain(struct boot_info* boot_info);

#endif
