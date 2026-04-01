void kmain(void);

void arch_serial_putchar(char c)
{
    __asm__ volatile("outb %0, %1" : : "a"(c), "Nd"((unsigned short)0x3F8));
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
    serial_write_string("Arx kernel: x86_64 entry\n");
    kmain();

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}
