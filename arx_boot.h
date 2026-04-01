#ifndef ARX_BOOT_H
#define ARX_BOOT_H

#include <stdint.h>

struct boot_info {
    uint64_t limine_present;
    uint64_t memmap_entry_count;
    uintptr_t memmap_entries;
    uint64_t framebuffer_addr;
    uint64_t framebuffer_width;
    uint64_t framebuffer_height;
    uint64_t framebuffer_pitch;
    uint64_t framebuffer_bpp;
};

struct boot_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

void kmain(struct boot_info *boot_info);

#endif
