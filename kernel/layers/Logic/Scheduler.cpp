#include "layers/Logic/Scheduler.hpp"

#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <platform.h>
}

Scheduler::Scheduler(ResourceLayerCaps* resourceLayerCaps)
{
    ResourceLayerImportCaps = resourceLayerCaps;

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        ReadyQueues[i].head = 0;
        ReadyQueues[i].tail = 0;
        ReadyQueues[i].count = 0;
        ReadyQueues[i].lock = 0;
    }

    InitializeBspProcessAndTask();
}

void Scheduler::InitializeBspProcessAndTask()
{
    if (ResourceLayerImportCaps == nullptr)
    {
        return;
    }

    if (ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr)
    {
        return;
    }

    ProcessManager* processManager = ResourceLayerImportCaps->processManager;
    TaskManager*    taskManager    = ResourceLayerImportCaps->taskManager;

    uint8_t cpuId = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return;
    }

    process_t* runningProcess = processManager->GetRunningProcess(cpuId);
    task_t*    runningTask    = taskManager->GetRunningTask(cpuId);
    if (runningProcess != nullptr && runningTask != nullptr)
    {
        return;
    }

    bool       createdProcess = false;
    process_t* process        = runningProcess;
    if (process == nullptr)
    {
        virt_addr_space_t* currentSpace = platform.cpus[cpuId].address_space;
        if (currentSpace == nullptr)
        {
            return;
        }

        process = processManager->CreateProcess(currentSpace);
        if (process == nullptr)
        {
            return;
        }

        createdProcess = true;
    }

    task_t* task = runningTask;
    if (task == nullptr)
    {
        task = taskManager->AllocateTask();
        if (task == nullptr)
        {
            if (createdProcess)
            {
                processManager->FreeProcess(process);
            }
            return;
        }

        if (!processManager->AddTask(process, task))
        {
            taskManager->FreeTask(task);
            if (createdProcess)
            {
                processManager->FreeProcess(process);
            }
            return;
        }
    }

    if (!processManager->SetRunningProcess(cpuId, process) || !taskManager->SetRunningTask(cpuId, task))
    {
        if (runningTask == nullptr)
        {
            taskManager->FreeTask(task);
        }
        if (createdProcess)
        {
            processManager->FreeProcess(process);
        }
    }
}

bool Scheduler::ScheduleProcess(uint64_t processId)
{
    if (ResourceLayerImportCaps == nullptr)
    {
        return false;
    }

    if (ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr)
    {
        return false;
    }

    ProcessManager* processManager = ResourceLayerImportCaps->processManager;
    TaskManager*    taskManager    = ResourceLayerImportCaps->taskManager;

    size_t count = processManager->GetCapacity();
    if (processId >= count)
    {
        return false;
    }

    process_t* table   = processManager->GetProcesses();
    process_t* process = &table[processId];
    if (!process->allocated)
    {
        return false;
    }

    task_t* firstTask = processManager->GetTasks(process);
    if (firstTask == nullptr)
    {
        return false;
    }

    uint8_t cpuId = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    process_t*        previousProcess      = processManager->GetRunningProcess(cpuId);
    virt_addr_space_t* previousAddressSpace = platform.cpus[cpuId].address_space;

    if (!processManager->ActivateProcessAddressSpace(process))
    {
        return false;
    }

    bool executed = taskManager->ExecuteTask(firstTask);

    if (processManager->GetRunningProcess(cpuId) != previousProcess)
    {
        (void) processManager->SetRunningProcess(cpuId, previousProcess);
    }

    if (platform.cpus[cpuId].address_space != previousAddressSpace)
    {
        platform.cpus[cpuId].address_space = previousAddressSpace;
        if (previousAddressSpace != nullptr)
        {
            vmm_switch_addr_space(previousAddressSpace);
        }
    }

    return executed;
}

bool Scheduler::EnqueueProcess(uint8_t cpuId, uint64_t processId)
{
    if (ResourceLayerImportCaps == nullptr)
    {
        return false;
    }

    if (ResourceLayerImportCaps->processManager == nullptr)
    {
        return false;
    }

    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    ProcessManager* processManager = ResourceLayerImportCaps->processManager;
    size_t          count          = processManager->GetCapacity();
    if (processId >= count)
    {
        return false;
    }

    process_t* table   = processManager->GetProcesses();
    process_t* process = &table[processId];
    if (!process->allocated)
    {
        return false;
    }

    task_t* firstTask = processManager->GetTasks(process);
    if (firstTask == nullptr)
    {
        return false;
    }

    ready_queue_t* queue = &ReadyQueues[cpuId];
    spinlock_acquire(&queue->lock);
    if (queue->count >= READY_QUEUE_CAPACITY)
    {
        spinlock_release(&queue->lock);
        return false;
    }

    queue->processIds[queue->tail] = processId;
    queue->tail                    = (queue->tail + 1) % READY_QUEUE_CAPACITY;
    queue->count++;
    spinlock_release(&queue->lock);
    return true;
}

bool Scheduler::RunNextReadyProcess(uint8_t cpuId)
{
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    if (cpuId != arch_cpu_id())
    {
        return false;
    }

    ready_queue_t* queue = &ReadyQueues[cpuId];
    uint64_t       processId;

    spinlock_acquire(&queue->lock);
    if (queue->count == 0)
    {
        spinlock_release(&queue->lock);
        return false;
    }

    processId   = queue->processIds[queue->head];
    queue->head = (queue->head + 1) % READY_QUEUE_CAPACITY;
    queue->count--;
    spinlock_release(&queue->lock);

    return ScheduleProcess(processId);
}
