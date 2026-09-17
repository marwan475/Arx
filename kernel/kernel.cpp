#include <arch/arch.h>
#include <klib/klib.h>

extern "C" void kmain(void)
{
    kterm_printf("Arx kernel: kmain entered on BSP\n");

    for (;;)
    {
        arch_pause();
    }
}
