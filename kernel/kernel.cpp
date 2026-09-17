#include <arch/arch.h>
#include <klib/klib.h>
#include "layers/Dispatcher.hpp"

extern "C" void kmain(void)
{
    Dispatcher* dispatcher = new Dispatcher();
    (void) dispatcher;

    kterm_printf("Arx kernel: kmain entered on BSP\n");

    for (;;)
    {
        arch_pause();
    }
}
