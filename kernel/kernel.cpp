#include "layers/Dispatcher.hpp"
#include "layers/Logic/Scheduler.hpp"
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
static void KernelPostInitTask(void* arg);
static bool BootstrapPostInitTasks(Dispatcher* dispatcher);

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

        if (mounted)
        {
            kprintf("Arx kernel: root initramfs mount ok\n");
        }
        else
        {
            error_logger("Arx kernel: root initramfs mount failed\n");
        }

        poststartkerneltests((void*) resourceLayerCaps, (void*) logicLayerCaps);
    }

    BootstrapPostInitTasks(dispatcher);

    __atomic_store_n(&platform.bsp_kmain_exited, 1, __ATOMIC_RELEASE);

    if (logicLayerCaps != nullptr && logicLayerCaps->scheduler != nullptr)
    {
        logicLayerCaps->scheduler->ActivateScheduling();
    }

    for (;;)
    {
        arch_pause();
    }
}

static void KernelPostInit(void)
{
#if defined(__x86_64__)
    (void) lapic_timer_calibrate();
#endif
    smp_selftests();

    if (arch_cpu_id() == platform.bsp_id)
    {
        Dispatcher* dispatcher = (Dispatcher*) platform.dispacher;

        if (dispatcher != nullptr)
        {
            launch_syscall_selftests((void*) dispatcher->GetRequestLayerCaps());
        }

        unsigned long long smpPasses   = 0;
        unsigned long long smpFails    = 0;
        unsigned long long smpFinished = 0;

        smp_selftests_wait_for_all_cpus();
        smp_selftests_get_totals(&smpPasses, &smpFails, &smpFinished);

        KDEBUG("smp summary: finished_cpus=%llu pass=%llu fail=%llu\n", smpFinished, smpPasses, smpFails);
        smp_selftests_kdebug_summary_details();

        selftest_group_begin("smp");
        selftest_case_begin("smp_selftests");

        if (smpFails > 0)
        {
            smp_selftests_record_failure_details();
        }

        selftest_case_end("smp_selftests", smpPasses, smpFails);
        selftest_group_end("smp");

        selftest_print_summary();
        error_logger_print();
    }

    kprintf("Arx kernel: cpu %u entered KernelPostInit\n", (unsigned) arch_cpu_id());
    KDEBUG("cpu %u entered KernelPostInit\n", (unsigned) arch_cpu_id());

    for (;;)
    {
        arch_pause();
    }
}

static void KernelPostInitTask(void* arg)
{
    (void) arg;
    KernelPostInit();
}

static bool BootstrapPostInitTasks(Dispatcher* dispatcher)
{
    if (dispatcher == nullptr)
    {
        kprintf("Arx kernel: bootstrap post-init tasks skipped (dispatcher null)\n");
        return false;
    }

    ResourceLayerCaps* resourceLayerCaps = dispatcher->GetResourceLayerCaps();
    LogicLayerCaps*    logicLayerCaps    = dispatcher->GetLogicLayerCaps();

    if (resourceLayerCaps == nullptr || logicLayerCaps == nullptr || resourceLayerCaps->processManager == nullptr || resourceLayerCaps->taskManager == nullptr || logicLayerCaps->scheduler == nullptr)
    {
        kprintf("Arx kernel: bootstrap post-init tasks skipped (missing caps)\n");
        return false;
    }

    ProcessManager* processManager = resourceLayerCaps->processManager;
    TaskManager*    taskManager    = resourceLayerCaps->taskManager;
    Scheduler*      scheduler      = logicLayerCaps->scheduler;

    bool all_ok = true;

    for (uint8_t cpuId = 0; cpuId < (uint8_t) platform.cpu_count; cpuId++)
    {
        virt_addr_space_t* activeSpace = platform.cpus[cpuId].address_space;

        if (activeSpace == nullptr && platform.bsp_id < platform.cpu_count)
        {
            activeSpace = platform.cpus[platform.bsp_id].address_space;
        }

        if (activeSpace == nullptr)
        {
            kprintf("Arx kernel: bootstrap cpu %u post-init process skipped (address space null)\n", (unsigned) cpuId);
            all_ok = false;
            continue;
        }

        process_t* process = processManager->CreateProcess(activeSpace);

        if (process == nullptr)
        {
            error_logger("Arx kernel: bootstrap cpu %u post-init process create failed\n", (unsigned) cpuId);
            all_ok = false;
            continue;
        }

        task_t* seedTask = taskManager->AllocateTask();

        if (seedTask == nullptr)
        {
            error_logger("Arx kernel: bootstrap cpu %u running task allocation failed\n", (unsigned) cpuId);
            (void) processManager->FreeProcess(process);
            all_ok = false;
            continue;
        }

        if (!taskManager->SetRunningTask(cpuId, seedTask))
        {
            error_logger("Arx kernel: bootstrap cpu %u running task set failed\n", (unsigned) cpuId);
            (void) taskManager->FreeTask(seedTask);
            (void) processManager->FreeProcess(process);
            all_ok = false;
            continue;
        }

        if (!processManager->SetRunningProcess(cpuId, process))
        {
            error_logger("Arx kernel: bootstrap cpu %u running process set failed\n", (unsigned) cpuId);
            (void) taskManager->SetRunningTask(cpuId, nullptr);
            (void) taskManager->FreeTask(seedTask);
            (void) processManager->FreeProcess(process);
            all_ok = false;
            continue;
        }

        task_t* postInitTask = taskManager->CreateKernelTask(KernelPostInitTask, (void*) (uintptr_t) cpuId);

        if (postInitTask == nullptr)
        {
            error_logger("Arx kernel: bootstrap cpu %u post-init task create failed\n", (unsigned) cpuId);
            all_ok = false;
            continue;
        }

        if (!processManager->AddTask(process, postInitTask))
        {
            error_logger("Arx kernel: bootstrap cpu %u post-init task attach failed\n", (unsigned) cpuId);
            (void) taskManager->FreeTask(postInitTask);
            all_ok = false;
            continue;
        }

        if (!scheduler->EnqueueProcess(cpuId, process->id))
        {
            error_logger("Arx kernel: bootstrap cpu %u post-init enqueue failed\n", (unsigned) cpuId);
            all_ok = false;
            continue;
        }

        kprintf("Arx kernel: bootstrap cpu %u queued post-init process=%llu task=%llu\n", (unsigned) cpuId, (unsigned long long) process->id, (unsigned long long) postInitTask->id);
    }

    return all_ok;
}
