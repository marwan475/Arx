#include "layers/Resource/ProcessManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <klib/intrusive_list.h>
#include <klib/klib.h>
#include <memory/vmm.h>
#include <platform.h>
}

ProcessManager::ProcessManager()
{
    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        RunningProcesses[i] = nullptr;
    }

    for (size_t i = 0; i < MAX_PROCESSES; i++)
    {
        Processes[i].allocated = false;
        Processes[i].id        = (uint64_t) i;
        Processes[i].addressSpace = nullptr;
        Processes[i].tasks = nullptr;
        Processes[i].fileDescriptors = nullptr;
        Processes[i].fileDescriptorCount = 0;
    }


}

process_t* ProcessManager::AllocateProcess()
{
    for (size_t i = 0; i < MAX_PROCESSES; i++)
    {
        if (!Processes[i].allocated)
        {
            Processes[i].allocated = true;
            Processes[i].id        = (uint64_t) i;
            Processes[i].addressSpace = nullptr;
            Processes[i].tasks = nullptr;
            Processes[i].fileDescriptors = nullptr;
            Processes[i].fileDescriptorCount = 0;
            return &Processes[i];
        }
    }

    return nullptr;
}

process_t* ProcessManager::CreateProcess(virt_addr_space_t* addressSpace)
{
    if (addressSpace == nullptr)
    {
        return nullptr;
    }

    process_t* process = AllocateProcess();
    if (process == nullptr)
    {
        return nullptr;
    }

    process->fileDescriptors = (file_descriptor_t*) kmalloc(sizeof(file_descriptor_t) * DEFAULT_FILE_DESCRIPTOR_COUNT);
    if (process->fileDescriptors == nullptr)
    {
        process->allocated = false;
        return nullptr;
    }

    memset(process->fileDescriptors, 0, sizeof(file_descriptor_t) * DEFAULT_FILE_DESCRIPTOR_COUNT);
    process->fileDescriptorCount = DEFAULT_FILE_DESCRIPTOR_COUNT;
    process->addressSpace = addressSpace;

    return process;
}

bool ProcessManager::FreeProcess(process_t* process)
{
    if (process == nullptr)
    {
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return false;
    }

    if (!process->allocated)
    {
        return false;
    }

    if (process->fileDescriptors != nullptr)
    {
        kfree(process->fileDescriptors);
        process->fileDescriptors = nullptr;
    }

    process->addressSpace = nullptr;
    process->id           = (uint64_t) (process - &Processes[0]);
    process->allocated    = false;
    process->tasks        = nullptr;
    process->fileDescriptorCount = 0;

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        if (RunningProcesses[i] == process)
        {
            RunningProcesses[i] = nullptr;
        }
    }

    return true;
}

bool ProcessManager::AddTask(process_t* process, task_t* task)
{
    if (process == nullptr || task == nullptr)
    {
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return false;
    }

    if (!process->allocated || !task->allocated)
    {
        return false;
    }

    for (task_t* iter = process->tasks; iter != nullptr; iter = iter->next)
    {
        if (iter == task)
        {
            return true;
        }
    }

    if (task->next != nullptr || task->prev != nullptr)
    {
        return false;
    }

    ILIST_APPEND(process->tasks, task);
    return true;
}

int64_t ProcessManager::AddFileDescriptor(process_t* process, file_handle_t file, uint32_t flags)
{
    if (process == nullptr || file == nullptr)
    {
        return -1;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return -1;
    }

    if (!process->allocated)
    {
        return -1;
    }

    if (process->fileDescriptors == nullptr || process->fileDescriptorCount == 0)
    {
        return -1;
    }

    for (uint64_t i = 0; i < process->fileDescriptorCount; i++)
    {
        if (process->fileDescriptors[i].file == nullptr)
        {
            process->fileDescriptors[i].file  = file;
            process->fileDescriptors[i].flags = flags;
            return (int64_t) i;
        }
    }

    const uint64_t oldCount = process->fileDescriptorCount;
    uint64_t newCount = oldCount * 2;
    if (newCount < oldCount)
    {
        return -1;
    }

    file_descriptor_t* newTable = (file_descriptor_t*) kmalloc(sizeof(file_descriptor_t) * newCount);
    if (newTable == nullptr)
    {
        return -1;
    }

    memcpy(newTable,
           process->fileDescriptors,
            sizeof(file_descriptor_t) * oldCount);

        memset(newTable + oldCount,
           0,
            sizeof(file_descriptor_t) * (newCount - oldCount));

    kfree(process->fileDescriptors);
    process->fileDescriptors   = newTable;
    process->fileDescriptorCount = newCount;

    process->fileDescriptors[oldCount].file  = file;
    process->fileDescriptors[oldCount].flags = flags;
    return (int64_t) oldCount;
}

file_handle_t ProcessManager::GetFileDescriptor(process_t* process, uint64_t fd, uint32_t* flagsOut) const
{
    if (process == nullptr)
    {
        return nullptr;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return nullptr;
    }

    if (!process->allocated || process->fileDescriptors == nullptr || fd >= process->fileDescriptorCount)
    {
        return nullptr;
    }

    file_handle_t file = process->fileDescriptors[fd].file;
    if (file == nullptr)
    {
        return nullptr;
    }

    if (flagsOut != nullptr)
    {
        *flagsOut = process->fileDescriptors[fd].flags;
    }

    return file;
}

bool ProcessManager::RemoveFileDescriptor(process_t* process, uint64_t fd, file_handle_t* fileOut, uint32_t* flagsOut)
{
    if (process == nullptr)
    {
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return false;
    }

    if (!process->allocated || process->fileDescriptors == nullptr || fd >= process->fileDescriptorCount)
    {
        return false;
    }

    file_handle_t file = process->fileDescriptors[fd].file;
    if (file == nullptr)
    {
        return false;
    }

    if (fileOut != nullptr)
    {
        *fileOut = file;
    }

    if (flagsOut != nullptr)
    {
        *flagsOut = process->fileDescriptors[fd].flags;
    }

    process->fileDescriptors[fd].file  = nullptr;
    process->fileDescriptors[fd].flags = FD_FLAG_NONE;
    return true;
}

bool ProcessManager::ActivateProcessAddressSpace(process_t* process)
{
    if (process == nullptr)
    {
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return false;
    }

    if (!process->allocated)
    {
        return false;
    }

    uint8_t cpuId = arch_cpu_id();
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    if (process->addressSpace == nullptr)
    {
        return false;
    }

    process_t* current = RunningProcesses[cpuId];
    if (current == nullptr)
    {
        RunningProcesses[cpuId] = process;
        vmm_switch_addr_space(process->addressSpace);
        return true;
    }

    if (current == process)
    {
        return true;
    }

    RunningProcesses[cpuId] = process;
    vmm_switch_addr_space(process->addressSpace);
    return true;
}

task_t* ProcessManager::GetTasks(process_t* process) const
{
    if (process == nullptr)
    {
        return nullptr;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return nullptr;
    }

    if (!process->allocated)
    {
        return nullptr;
    }

    return process->tasks;
}

process_t* ProcessManager::GetRunningProcess(uint8_t cpuId) const
{
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return nullptr;
    }

    return RunningProcesses[cpuId];
}

process_t* ProcessManager::GetCurrentProcess() const
{
    return GetRunningProcess(arch_cpu_id());
}

bool ProcessManager::SetRunningProcess(uint8_t cpuId, process_t* process)
{
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    if (process != nullptr)
    {
        if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
        {
            return false;
        }

        if (!process->allocated)
        {
            return false;
        }
    }

    RunningProcesses[cpuId] = process;
    return true;
}

process_t* ProcessManager::GetProcesses()
{
    return Processes;
}

const process_t* ProcessManager::GetProcesses() const
{
    return Processes;
}

size_t ProcessManager::GetCapacity() const
{
    return MAX_PROCESSES;
}
