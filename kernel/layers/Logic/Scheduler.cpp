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
        SchedulingActive[i] = false;
    }

    InitializeBspProcessAndTask();
}

void Scheduler::ActivateScheduling()
{
    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        SchedulingActive[i] = true;
    }
}

bool Scheduler::IsSchedulingActive() const
{
    const uint8_t cpuId = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    return SchedulingActive[cpuId];
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

bool Scheduler::RemoveProcessFromQueue(ready_queue_t* queue, uint64_t processId)
{
    if (queue == nullptr || queue->count == 0)
    {
        return false;
    }

    bool   removed  = false;
    size_t original = queue->count;
    size_t readPos  = queue->head;

    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;

    for (size_t i = 0; i < original; ++i)
    {
        const uint64_t queuedProcess = queue->processIds[readPos];
        readPos                      = (readPos + 1) % READY_QUEUE_CAPACITY;

        if (queuedProcess == processId)
        {
            removed = true;
            continue;
        }

        queue->processIds[queue->tail] = queuedProcess;
        queue->tail                    = (queue->tail + 1) % READY_QUEUE_CAPACITY;
        queue->count++;
    }

    return removed;
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
    if (!process->allocated || process->exited)
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
    if (!process->allocated || process->exited)
    {
        return false;
    }

    task_t* firstTask = processManager->GetTasks(process);
    if (firstTask == nullptr)
    {
        return false;
    }

    // Invariant: one process may be queued in at most one ready queue.
    for (uint8_t otherCpu = 0; otherCpu < BOOT_SMP_MAX_CPUS; ++otherCpu)
    {
        ready_queue_t* otherQueue = &ReadyQueues[otherCpu];
        size_t         cursor     = otherQueue->head;
        for (size_t i = 0; i < otherQueue->count; ++i)
        {
            if (otherQueue->processIds[cursor] == processId)
            {
                return false;
            }
            cursor = (cursor + 1) % READY_QUEUE_CAPACITY;
        }
    }

    ready_queue_t* queue = &ReadyQueues[cpuId];
    if (queue->count >= READY_QUEUE_CAPACITY)
    {
        return false;
    }

    queue->processIds[queue->tail] = processId;
    queue->tail                    = (queue->tail + 1) % READY_QUEUE_CAPACITY;
    queue->count++;
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
    while (queue->count > 0)
    {
        const uint64_t processId = queue->processIds[queue->head];
        queue->head              = (queue->head + 1) % READY_QUEUE_CAPACITY;
        queue->count--;

        if (ScheduleProcess(processId))
        {
            return true;
        }
    }

    return false;
}

bool Scheduler::KillProcess(uint64_t processId, int32_t exitStatus)
{
    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr)
    {
        return false;
    }

    ProcessManager* processManager = ResourceLayerImportCaps->processManager;
    TaskManager*    taskManager    = ResourceLayerImportCaps->taskManager;
    const size_t    capacity       = processManager->GetCapacity();
    if (processId >= capacity)
    {
        return false;
    }

    process_t* table   = processManager->GetProcesses();
    process_t* process = &table[processId];
    if (!process->allocated)
    {
        return false;
    }

    process->exited     = true;
    process->exitStatus = exitStatus;

    for (uint8_t cpu = 0; cpu < BOOT_SMP_MAX_CPUS; ++cpu)
    {
        if (RemoveProcessFromQueue(&ReadyQueues[cpu], processId))
        {
            break;
        }
    }

    // Best-effort teardown: free process tasks that are not currently running.
    // Running-task teardown needs a dedicated context-switch-away path.
    task_t* iter = process->tasks;
    while (iter != nullptr)
    {
        task_t* next = iter->next;

        bool isRunning = false;
        for (uint8_t cpu = 0; cpu < BOOT_SMP_MAX_CPUS; ++cpu)
        {
            if (taskManager->GetRunningTask(cpu) == iter)
            {
                isRunning = true;
                break;
            }
        }

        if (!isRunning)
        {
            if (iter->prev != nullptr)
            {
                iter->prev->next = iter->next;
            }
            else
            {
                process->tasks = iter->next;
            }

            if (iter->next != nullptr)
            {
                iter->next->prev = iter->prev;
            }

            iter->next = nullptr;
            iter->prev = nullptr;
            (void) taskManager->FreeTask(iter);
        }

        iter = next;
    }

    (void) processManager->TryReapExitedProcess(processId);

    return true;
}
