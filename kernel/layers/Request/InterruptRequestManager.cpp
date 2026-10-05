#include "layers/Request/InterruptRequestManager.hpp"

#include "layers/Dispatcher.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/ProcessManager.hpp"

#include <arch/arch.h>
#include <platform.h>

InterruptRequestManager::InterruptRequestManager()
{
    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        ScheduleTickCounters[i] = 0;
    }
}

void InterruptRequestManager::HandleScheduleRequest()
{
    const uint8_t cpuId = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return;
    }

    ScheduleTickCounters[cpuId]++;
    if (ScheduleTickCounters[cpuId] < SCHEDULE_TICK_INTERVAL)
    {
        return;
    }
    ScheduleTickCounters[cpuId] = 0;

    Dispatcher* dispatcher = (Dispatcher*) platform.dispacher;
    if (dispatcher == nullptr)
    {
        return;
    }

    LogicLayerCaps*    logicLayerCaps    = dispatcher->GetLogicLayerCaps();
    ResourceLayerCaps* resourceLayerCaps = dispatcher->GetResourceLayerCaps();
    if (logicLayerCaps == nullptr || logicLayerCaps->scheduler == nullptr || resourceLayerCaps == nullptr || resourceLayerCaps->processManager == nullptr)
    {
        return;
    }

    ProcessManager* processManager = resourceLayerCaps->processManager;
    Scheduler*      scheduler      = logicLayerCaps->scheduler;

    if (!scheduler->IsSchedulingActive())
    {
        return;
    }

    process_t* currentProcess = processManager->GetCurrentProcess();
    if (currentProcess != nullptr)
    {
        (void) scheduler->EnqueueProcess(cpuId, currentProcess->id);
    }

    (void) scheduler->RunNextReadyProcess(cpuId);
}

void InterruptRequestManager::HandleInterruptRequest(uint64_t requestNumber)
{
    switch (requestNumber)
    {
        case INTERRUPT_REQUEST_SCHEDULE:
            HandleScheduleRequest();
            return;
        default:
            return;
    }
}