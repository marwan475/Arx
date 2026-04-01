void kmain(void);

void arch_serial_putchar(char c)
{
    volatile unsigned int *const pl011_dr = (volatile unsigned int *)0x09000000;
    volatile unsigned int *const pl011_fr = (volatile unsigned int *)0x09000018;

    while ((*pl011_fr) & (1u << 5))
    {
    }

    *pl011_dr = (unsigned int)c;
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
    serial_write_string("Arx kernel: aarch64 entry\n");
    kmain();

    for (;;)
    {
        __asm__ volatile("wfi");
    }
}
