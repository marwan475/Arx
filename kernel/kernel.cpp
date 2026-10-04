#include "layers/Dispatcher.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"

#include <arch/arch.h>
#include <klib/klib.h>
#include <platform.h>

extern "C"
{
#include <selftests/selftests.h>
}

static void KernelPostInit(void);

extern "C" void kmain(void)
{
    Dispatcher* dispatcher = new Dispatcher();
    platform.dispacher     = dispatcher;

    kterm_printf("Arx kernel: kmain entered on BSP\n");

    dispatcher->StartKernel();

    kterm_printf("Arx kernel: StartKernel completed on BSP\n");

    ResourceLayerCaps* resourceLayerCaps = dispatcher->GetResourceLayerCaps();
    LogicLayerCaps*    logicLayerCaps    = dispatcher->GetLogicLayerCaps();

    if (resourceLayerCaps != nullptr && logicLayerCaps != nullptr && logicLayerCaps->virtualFileSystem != nullptr)
    {
        const bool mounted = logicLayerCaps->virtualFileSystem->MountRootFileSystem("cpio", resourceLayerCaps->initRamFileSystemManager);
        kprintf("Arx kernel: root initramfs mount %s\n", mounted ? "ok" : "failed");
        poststartkerneltests((void*) resourceLayerCaps, (void*) logicLayerCaps);
    }

    selftest_print_summary();

    platform.bsp_kmain_exited = 1;

    KernelPostInit();
}

extern "C" void smp_kmain(void)
{
    kterm_printf("Arx kernel: cpu %u entered smp_kmain wait\n", (unsigned) arch_cpu_id());

    while (platform.bsp_kmain_exited == 0)
    {
        arch_pause();
    }

    kterm_printf("Arx kernel: cpu %u observed BSP exit from kmain\n", (unsigned) arch_cpu_id());

    KernelPostInit();
}

static void KernelPostInit(void)
{

    kprintf("Arx kernel: cpu %u entered KernelPostInit\n", (unsigned) arch_cpu_id());
    for (;;)
    {
        arch_pause();
    }
}
