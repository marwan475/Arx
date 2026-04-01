#include <boot/boot.h>
#include <boot/limine.h>
#include <stdint.h>

__attribute__((used, section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests"))) static volatile struct limine_memmap_request memmap_request = {
        .id       = LIMINE_MEMMAP_REQUEST,
        .revision = 0,
        .response = 0,
};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_framebuffer_request framebuffer_request = {
        .id       = LIMINE_FRAMEBUFFER_REQUEST,
        .revision = 0,
        .response = 0,
};

__attribute__((used, section(".limine_requests_start"))) static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;

static struct boot_memmap_entry boot_memmap[BOOT_MEMMAP_MAX_ENTRIES];

void arch_serial_putchar(char c)
{
    __asm__ volatile("outb %0, %1" : : "a"(c), "Nd"((unsigned short) 0x3F8));
}

static void serial_write_string(const char* s)
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

static void gather_boot_info(struct boot_info* boot_info)
{
    boot_info->limine_present     = 0;
    boot_info->memmap_entry_count = 0;
    boot_info->memmap_entries     = 0;
    boot_info->framebuffer_addr   = 0;
    boot_info->framebuffer_width  = 0;
    boot_info->framebuffer_height = 0;
    boot_info->framebuffer_pitch  = 0;
    boot_info->framebuffer_bpp    = 0;

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
            struct limine_memmap_entry* entry = memmap_request.response->entries[i];
            boot_memmap[i].base               = entry->base;
            boot_memmap[i].length             = entry->length;
            boot_memmap[i].type               = entry->type;
        }

        boot_info->memmap_entry_count = count;
        boot_info->memmap_entries     = (uintptr_t) boot_memmap;
    }

    if (framebuffer_request.response != 0 && framebuffer_request.response->framebuffer_count > 0)
    {
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

        boot_info->framebuffer_addr   = (uint64_t) (uintptr_t) fb->address;
        boot_info->framebuffer_width  = fb->width;
        boot_info->framebuffer_height = fb->height;
        boot_info->framebuffer_pitch  = fb->pitch;
        boot_info->framebuffer_bpp    = fb->bpp;
    }
}

void _start(void)
{
    struct boot_info boot_info;

    serial_write_string("Arx kernel: x86_64 entry\n");
    gather_boot_info(&boot_info);
    kmain(&boot_info);

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
