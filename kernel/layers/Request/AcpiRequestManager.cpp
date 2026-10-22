#include "layers/Request/AcpiRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <klib/klib.h>
#include <platform.h>

uint64_t AcpiRequestManager::ThreadId(ResourceLayerCaps* resources)
{
    if (resources == nullptr || resources->taskManager == nullptr)
    {
        return 0;
    }

    cpu_preemption_disable();
    task_t* task = resources->taskManager->GetCurrentTask();
    uint64_t identity = task != nullptr ? task->id + 1 : 0;
    cpu_preemption_enable();
    return identity;
}

bool AcpiRequestManager::CanWait(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    return resources != nullptr && resources->taskManager != nullptr && logic != nullptr && logic->scheduler != nullptr &&
           logic->scheduler->IsSchedulingActive() && arch_interrupts_enabled() && platform.cpus[arch_cpu_id()].preemption_depth == 0 &&
           resources->taskManager->GetCurrentTask() != nullptr;
}

bool AcpiRequestManager::Wait(ResourceLayerCaps* resources, LogicLayerCaps* logic, const uint64_t* sequence, uint64_t expected, uint64_t deadline)
{
    if (sequence == nullptr || !CanWait(resources, logic))
    {
        return false;
    }

    uint64_t result = logic->scheduler->WaitForEvent(sequence, expected, deadline, true);
    return result == 0 || result == static_cast<uint64_t>(-110);
}

void AcpiRequestManager::Wake(LogicLayerCaps* logic, const uint64_t* sequence)
{
    if (logic != nullptr && logic->scheduler != nullptr && sequence != nullptr)
    {
        logic->scheduler->WakeEventWaiters(sequence);
    }
}

bool AcpiRequestManager::CreateWorker(ResourceLayerCaps* resources, LogicLayerCaps* logic, void (*entry)(void*), void* argument)
{
    if (resources == nullptr || logic == nullptr || resources->processManager == nullptr || resources->taskManager == nullptr || logic->scheduler == nullptr || entry == nullptr ||
        arch_cpu_id() != platform.bsp_id || platform.bsp_id >= 64)
    {
        return false;
    }

    ProcessManager* processes = resources->processManager;
    TaskManager* tasks = resources->taskManager;
    Scheduler* scheduler = logic->scheduler;
    VirtualMemoryManager* memory = resources->virtualMemoryManager;

    if (memory == nullptr)
    {
        return false;
    }

    // Blocking scheduler waits pin their process memory. Private roots prevent
    // lifetime firmware workers from pinning the shared bootstrap root.
    virt_addr_space_t* space = memory->CreateUserAddressSpace();

    if (space == nullptr)
    {
        return false;
    }

    process_t* process = processes->CreateProcess(space);

    if (process == nullptr)
    {
        memory->DestroyUserAddressSpace(space);
        return false;
    }

    process->ownsAddressSpace = true;

    task_t* task = tasks->CreateKernelTask(entry, argument);

    if (task == nullptr)
    {
        processes->FreeProcess(process);
        return false;
    }

    if (scheduler->SetAffinity(task, 1ULL << platform.bsp_id) != 0 || !scheduler->PublishTask(process, task))
    {
        tasks->FreeTask(task);
        processes->FreeProcess(process);
        return false;
    }

    return true;
}
