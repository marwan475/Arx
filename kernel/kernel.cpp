#include "layers/Dispatcher.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TaskManager.hpp"

#include <arch/arch.h>
#include <klib/klib.h>
#include <platform.h>

extern "C"
{
#include <selftests/selftests.h>
}

static void KernelPostInit(void);
static void KernelPostInitCreateApProcess(void);
void        smp_selftests(void);

extern "C" void kmain(void)
{
    Dispatcher* dispatcher = new Dispatcher();
    platform.dispacher     = dispatcher;

    kterm_printf("Arx kernel: kmain entered on BSP\n");

    dispatcher->StartKernel();

    kterm_printf("Arx kernel: StartKernel completed on BSP\n");

    ResourceLayerCaps* resourceLayerCaps = dispatcher->GetResourceLayerCaps();
    LogicLayerCaps*    logicLayerCaps    = dispatcher->GetLogicLayerCaps();

    if (resourceLayerCaps != nullptr && resourceLayerCaps->processManager != nullptr && resourceLayerCaps->taskManager != nullptr)
    {
        ProcessManager*   processManager = resourceLayerCaps->processManager;
        TaskManager*      taskManager    = resourceLayerCaps->taskManager;
        virt_addr_space_t* activeSpace   = nullptr;
        task_t*           runningTask    = taskManager->GetRunningTask((uint8_t) platform.bsp_id);

        if (platform.bsp_id < platform.cpu_count)
        {
            activeSpace = platform.cpus[platform.bsp_id].address_space;
        }

        if (activeSpace == nullptr)
        {
            kprintf("Arx kernel: cpu %u kmain process create skipped (address space null)\n", (unsigned) platform.bsp_id);
        }
        else
        {
            process_t* process = processManager->CreateProcess(activeSpace);
            if (process == nullptr)
            {
                kprintf("Arx kernel: cpu %u kmain process create failed\n", (unsigned) platform.bsp_id);
            }
            else if (!processManager->SetRunningProcess((uint8_t) platform.bsp_id, process))
            {
                kprintf("Arx kernel: cpu %u kmain process set-running failed\n", (unsigned) platform.bsp_id);
                (void) processManager->FreeProcess(process);
            }
            else
            {
                kprintf("Arx kernel: cpu %u kmain process created id=%llu\n", (unsigned) platform.bsp_id, (unsigned long long) process->id);

                if (runningTask == nullptr)
                {
                    runningTask = taskManager->AllocateTask();
                    if (runningTask == nullptr)
                    {
                        kprintf("Arx kernel: cpu %u kmain running task allocation failed\n", (unsigned) platform.bsp_id);
                    }
                    else if (!taskManager->SetRunningTask((uint8_t) platform.bsp_id, runningTask))
                    {
                        kprintf("Arx kernel: cpu %u kmain running task set failed\n", (unsigned) platform.bsp_id);
                        (void) taskManager->FreeTask(runningTask);
                    }
                    else
                    {
                        kprintf("Arx kernel: cpu %u kmain running task created id=%llu\n", (unsigned) platform.bsp_id, (unsigned long long) runningTask->id);
                    }
                }
            }
        }
    }

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
    KernelPostInitCreateApProcess();
    smp_selftests();

    kprintf("Arx kernel: cpu %u entered KernelPostInit\n", (unsigned) arch_cpu_id());
    for (;;)
    {
        arch_pause();
    }
}

static void KernelPostInitCreateApProcess(void)
{
    const uint64_t cpu_id = (uint64_t) arch_cpu_id();

    if (cpu_id == platform.bsp_id)
    {
        return;
    }

    Dispatcher* dispatcher = (Dispatcher*) platform.dispacher;
    if (dispatcher == nullptr)
    {
        kprintf("Arx kernel: cpu %u KernelPostInit AP process skipped (dispatcher null)\n", (unsigned) cpu_id);
        return;
    }

    ResourceLayerCaps* resourceLayerCaps = dispatcher->GetResourceLayerCaps();
    if (resourceLayerCaps == nullptr || resourceLayerCaps->processManager == nullptr || resourceLayerCaps->taskManager == nullptr)
    {
        kprintf("Arx kernel: cpu %u KernelPostInit AP process skipped (process/task manager null)\n", (unsigned) cpu_id);
        return;
    }

    ProcessManager* processManager = resourceLayerCaps->processManager;
    TaskManager*    taskManager    = resourceLayerCaps->taskManager;
    virt_addr_space_t* activeSpace = platform.cpus[cpu_id].address_space;
    task_t*         runningTask    = taskManager->GetRunningTask((uint8_t) cpu_id);

    if (activeSpace == nullptr && platform.bsp_id < platform.cpu_count)
    {
        activeSpace = platform.cpus[platform.bsp_id].address_space;
    }

    if (activeSpace == nullptr)
    {
        kprintf("Arx kernel: cpu %u KernelPostInit AP process skipped (address space null)\n", (unsigned) cpu_id);
        return;
    }

    process_t* process = processManager->CreateProcess(activeSpace);
    if (process == nullptr)
    {
        kprintf("Arx kernel: cpu %u KernelPostInit AP process create failed\n", (unsigned) cpu_id);
        return;
    }

    if (!processManager->SetRunningProcess((uint8_t) cpu_id, process))
    {
        kprintf("Arx kernel: cpu %u KernelPostInit AP process set-running failed\n", (unsigned) cpu_id);
        (void) processManager->FreeProcess(process);
        return;
    }

    if (runningTask == nullptr)
    {
        runningTask = taskManager->AllocateTask();
        if (runningTask == nullptr)
        {
            kprintf("Arx kernel: cpu %u KernelPostInit AP running task allocation failed\n", (unsigned) cpu_id);
            (void) processManager->FreeProcess(process);
            return;
        }

        if (!taskManager->SetRunningTask((uint8_t) cpu_id, runningTask))
        {
            kprintf("Arx kernel: cpu %u KernelPostInit AP running task set failed\n", (unsigned) cpu_id);
            (void) taskManager->FreeTask(runningTask);
            (void) processManager->FreeProcess(process);
            return;
        }

        kprintf("Arx kernel: cpu %u KernelPostInit AP running task created id=%llu\n", (unsigned) cpu_id, (unsigned long long) runningTask->id);
    }

    kprintf("Arx kernel: cpu %u KernelPostInit AP process created id=%llu\n", (unsigned) cpu_id, (unsigned long long) process->id);
}
