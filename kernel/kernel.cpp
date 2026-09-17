#include "layers/Dispatcher.hpp"

#include <arch/arch.h>
#include <klib/klib.h>
#include <platform.h>

extern "C" void kmain(void)
{
    Dispatcher* dispatcher = new Dispatcher();
    platform.dispacher     = dispatcher;

    kterm_printf("Arx kernel: kmain entered on BSP\n");

    dispatcher->StartKernel();

    kterm_printf("Arx kernel: StartKernel completed on BSP\n");

    for (;;)
    {
        arch_pause();
    }
}
