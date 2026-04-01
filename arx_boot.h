#ifndef ARX_BOOT_H
#define ARX_BOOT_H

#include <stdint.h>

struct boot_info {
    uint64_t limine_present;
    uint64_t memmap_entry_count;
    uint64_t framebuffer_addr;
    uint64_t framebuffer_width;
    uint64_t framebuffer_height;
    uint64_t framebuffer_pitch;
    uint64_t framebuffer_bpp;
};

void kmain(struct boot_info *boot_info);

#endif
