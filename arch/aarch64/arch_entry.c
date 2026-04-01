#include "../../arx_boot.h"

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

void _start(void)
{
    struct boot_info boot_info = {0};

    serial_write_string("Arx kernel: aarch64 entry\n");
    kmain(&boot_info);

    for (;;)
    {
        __asm__ volatile("wfi");
    }
}
