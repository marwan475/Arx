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

void kmain(void)
{
    serial_write_string("Arx kernel: kmain online\n");

    for (;;)
    {
    }
}
