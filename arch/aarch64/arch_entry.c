#include <stdint.h>

#include "../../boot/boot.h"
#include "../../boot/limine.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

#define BOOT_MEMMAP_MAX_ENTRIES 256

static struct boot_memmap_entry boot_memmap[BOOT_MEMMAP_MAX_ENTRIES];

static uintptr_t pl011_base = 0x09000000u;

static inline volatile unsigned int *pl011_reg(unsigned int offset)
{
    return (volatile unsigned int *)(pl011_base + offset);
}

static void arch_serial_init(void)
{
    static int initialized = 0;

    if (initialized)
    {
        return;
    }

    if (hhdm_request.response != 0)
    {
        pl011_base = (uintptr_t)(hhdm_request.response->offset + 0x09000000u);
    }

    *pl011_reg(0x30) = 0;
    *pl011_reg(0x44) = 0x7ffu;

    *pl011_reg(0x24) = 13;
    *pl011_reg(0x28) = 1;

    *pl011_reg(0x2c) = (1u << 4) | (3u << 5);
    *pl011_reg(0x30) = (1u << 0) | (1u << 8) | (1u << 9);

    initialized = 1;
}

void arch_serial_putchar(char c)
{
    arch_serial_init();

    for (unsigned int i = 0; i < 1000000; i++)
    {
        if (((*pl011_reg(0x18)) & (1u << 5)) == 0)
        {
            break;
        }
    }

    *pl011_reg(0x00) = (unsigned int)c;
}

static void serial_write_string(const char *s)
{
    while (*s != '\0')
    {
        if (*s == '\n')
        {
            arch_serial_putchar('\r');
        }

        arch_serial_putchar(*s);
        s++;
    }
}

static void gather_boot_info(struct boot_info *boot_info)
{
    boot_info->limine_present = 0;
    boot_info->memmap_entry_count = 0;
    boot_info->memmap_entries = 0;
    boot_info->framebuffer_addr = 0;
    boot_info->framebuffer_width = 0;
    boot_info->framebuffer_height = 0;
    boot_info->framebuffer_pitch = 0;
    boot_info->framebuffer_bpp = 0;

    if (!LIMINE_BASE_REVISION_SUPPORTED)
    {
        return;
    }

    boot_info->limine_present = 1;

    if (memmap_request.response != 0)
    {
        uint64_t count = memmap_request.response->entry_count;
        if (count > BOOT_MEMMAP_MAX_ENTRIES)
        {
            count = BOOT_MEMMAP_MAX_ENTRIES;
        }

        for (uint64_t i = 0; i < count; i++)
        {
            struct limine_memmap_entry *entry = memmap_request.response->entries[i];
            boot_memmap[i].base = entry->base;
            boot_memmap[i].length = entry->length;
            boot_memmap[i].type = entry->type;
        }

        boot_info->memmap_entry_count = count;
        boot_info->memmap_entries = (uintptr_t)boot_memmap;
    }

    if (framebuffer_request.response != 0 && framebuffer_request.response->framebuffer_count > 0)
    {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

        boot_info->framebuffer_addr = (uint64_t)(uintptr_t)fb->address;
        boot_info->framebuffer_width = fb->width;
        boot_info->framebuffer_height = fb->height;
        boot_info->framebuffer_pitch = fb->pitch;
        boot_info->framebuffer_bpp = fb->bpp;
    }
}

void _start(void)
{
    struct boot_info boot_info;

    serial_write_string("Arx kernel: aarch64 entry\n");
    gather_boot_info(&boot_info);
    kmain(&boot_info);

    for (;;)
    {
        __asm__ volatile("wfi");
    }
}
