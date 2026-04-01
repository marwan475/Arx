#include <stdint.h>

#include <boot/boot.h>

void arch_serial_putchar(char c);

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

static void serial_write_dec_u64(uint64_t value)
{
    char buf[21];
    int i = 0;

    if (value == 0)
    {
        arch_serial_putchar('0');
        return;
    }

    while (value > 0)
    {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0)
    {
        arch_serial_putchar(buf[--i]);
    }
}

static void serial_write_hex_u64(uint64_t value)
{
    static const char hex[] = "0123456789abcdef";

    serial_write_string("0x");

    for (int shift = 60; shift >= 0; shift -= 4)
    {
        arch_serial_putchar(hex[(value >> shift) & 0xfu]);
    }
}

static void panic_halt(void)
{
    for (;;)
    {
    }
}

static const char *boot_memmap_type_to_string(uint64_t type)
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

void kmain(struct boot_info *boot_info)
{
    struct boot_memmap_entry *memmap;

    serial_write_string("Arx kernel: kmain online\n");

    if (boot_info == 0 || boot_info->limine_present == 0)
    {
        serial_write_string("Arx kernel: no boot protocol info\n");
        panic_halt();
    }

    if (boot_info->memmap_entries == 0 || boot_info->memmap_entry_count == 0)
    {
        serial_write_string("Arx kernel: no Limine memory map entries\n");
        panic_halt();
    }

    serial_write_string("Arx kernel: memmap entries=");
    serial_write_dec_u64(boot_info->memmap_entry_count);
    serial_write_string("\n");

    memmap = (struct boot_memmap_entry *)(uintptr_t)boot_info->memmap_entries;
    for (uint64_t i = 0; i < boot_info->memmap_entry_count; i++)
    {
        serial_write_string("Arx kernel: memmap[");
        serial_write_dec_u64(i);
        serial_write_string("] base=");
        serial_write_hex_u64(memmap[i].base);
        serial_write_string(" len=");
        serial_write_hex_u64(memmap[i].length);
        serial_write_string(" type=");
        serial_write_string(boot_memmap_type_to_string(memmap[i].type));
        serial_write_string("\n");
    }

    if (boot_info->framebuffer_addr == 0)
    {
        serial_write_string("Arx kernel: no Limine framebuffer response\n");
        panic_halt();
    }

    serial_write_string("Arx kernel: framebuffer addr=");
    serial_write_hex_u64(boot_info->framebuffer_addr);
    serial_write_string(" size=");
    serial_write_dec_u64(boot_info->framebuffer_width);
    serial_write_string("x");
    serial_write_dec_u64(boot_info->framebuffer_height);
    serial_write_string(" pitch=");
    serial_write_dec_u64(boot_info->framebuffer_pitch);
    serial_write_string(" bpp=");
    serial_write_dec_u64(boot_info->framebuffer_bpp);
    serial_write_string("\n");

    for (;;)
    {
    }
}
