#include "layers/Dispatcher.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/PciManager.hpp"
#include "layers/Resource/AcpiManager.hpp"
#include "layers/Resource/TaskManager.hpp"

#include <arch/arch.h>
#include <klib/klib.h>
#include <debug/kernel/Foundation.hpp>
#include <platform.h>

#include <selftests/SelfTestManager.hpp>

static void KernelPostInit(void);
static void KernelPostInitTask(void* arg);
static bool BootstrapPostInitTasks(Dispatcher* dispatcher);

extern "C" void kmain(void)
{
    Dispatcher* dispatcher = new Dispatcher();
    platform.dispacher     = dispatcher;
    ARX_DEBUG_STORE(platform.debugStats.dispatcherPublished, 1);
    ARX_DEBUG_STORE(platform.debugStats.stage, ARX_PLATFORM_DISPATCHER_PUBLISHED);

    kterm_printf("Arx kernel: kmain entered on BSP\n");

    dispatcher->StartKernel();

    kterm_printf("Arx kernel: StartKernel completed on BSP\n");

    ResourceLayerCaps* resourceLayerCaps = dispatcher->GetResourceLayerCaps();
    LogicLayerCaps*    logicLayerCaps    = dispatcher->GetLogicLayerCaps();
    bool               mounted          = false;

    if (resourceLayerCaps != nullptr && resourceLayerCaps->acpiManager != nullptr)
    {
        acpi_runtime_state_t state = resourceLayerCaps->acpiManager->Initialize();
        kprintf("Arx kernel: ACPI status=%u stage=%u devices=%llu registration=%u\n",
                (unsigned) state.status, (unsigned) state.stage,
                (unsigned long long) state.device_count,
                (unsigned) resourceLayerCaps->acpiManager->GetRegistrationStatus());
    }

    if (resourceLayerCaps != nullptr && resourceLayerCaps->pciManager != nullptr)
    {
        PciStatus status = resourceLayerCaps->pciManager->EnumerateDevices();

        kprintf("Arx kernel: PCI enumeration status=%u devices=%llu truncated=%u registration=%u controller=%llu\n",
                (unsigned) status,
                (unsigned long long) resourceLayerCaps->pciManager->CountDevices(),
                (unsigned) resourceLayerCaps->pciManager->IsTruncated(),
                (unsigned) resourceLayerCaps->pciManager->GetRegistrationStatus(),
                (unsigned long long) resourceLayerCaps->pciManager->GetControllerDeviceId());

        debug_pci_devices();
    }

    SelfTestManager::Get().RunPciDiscoveryTests(resourceLayerCaps);

    if (resourceLayerCaps != nullptr && logicLayerCaps != nullptr && logicLayerCaps->virtualFileSystem != nullptr)
    {
        mounted = logicLayerCaps->virtualFileSystem->MountRootFileSystem("cpio", resourceLayerCaps->initRamFileSystemManager);

        if (mounted)
        {
            kprintf("Arx kernel: root initramfs mount ok\n");
        }
        else
        {
            error_logger("Arx kernel: root initramfs mount failed\n");
        }
    }

    SelfTestManager::Get().SetRootFileSystemMounted(mounted);

    if (!BootstrapPostInitTasks(dispatcher))
    {
        SelfTestManager::Get().AbortUnsafe("post-init task creation or enqueue failed");
    }

    if (platform_acpi_state().stage == ACPI_STAGE_READY && !platform_acpi_start_workers())
    {
        error_logger("ACPI: worker handoff failed; continuing with firmware tables only\n");
        resourceLayerCaps->acpiManager->Initialize();
    }

    ARX_DEBUG_STORE(platform.debugStats.bspHandoff, 1);
    ARX_DEBUG_STORE(platform.debugStats.stage, ARX_PLATFORM_BSP_HANDOFF);
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
    arch_calibrate_scheduler_timer();
    SelfTestManager::Get().RunPostInitTests();

    if (ARX_ACPI_TEST_ACTION != 0 && arch_cpu_id() == platform.bsp_id)
    {
        Dispatcher* dispatcher = static_cast<Dispatcher*>(platform.dispacher);
        AcpiManager* acpi = dispatcher->GetResourceLayerCaps()->acpiManager;
        kprintf("ACPI TEST: final firmware action=%u\n", (unsigned) ARX_ACPI_TEST_ACTION);
        acpi_status_t status = ARX_ACPI_TEST_ACTION == 1 ? acpi->PowerOff() : acpi->Reset();
        error_logger("ACPI TEST: final firmware action returned status=%u\n", (unsigned) status);
    }

    kprintf("Arx kernel: cpu %u entered KernelPostInit\n", (unsigned) arch_cpu_id());
    KDEBUG("cpu %u entered KernelPostInit\n", (unsigned) arch_cpu_id());

    for (;;)
    {
        arch_halt();
    }
}

static void KernelPostInitTask(void* arg)
{
    (void) arg;
    ARX_DEBUG_RUN(DebugFoundationRuntimeUpdate());
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
