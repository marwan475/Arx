#include "layers/Resource/ProcessManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <klib/intrusive_list.h>
#include <klib/klib.h>
#include <memory/vmm.h>
#include <platform.h>
}

namespace
{
static bool add_would_overflow_u64(uint64_t a, uint64_t b)
{
    return a > (UINT64_MAX - b);
}

static bool copy_to_process_virtual(virt_addr_space_t* addressSpace, uint64_t destinationVirtualAddress, const void* source, uint64_t size)
{
    if (addressSpace == nullptr || source == nullptr)
    {
        return false;
    }

    const uint8_t* sourceBytes = (const uint8_t*) source;
    uint64_t       offset      = 0;

    while (offset < size)
    {
        const uint64_t currentVirtualAddress = destinationVirtualAddress + offset;
        const uint64_t pageOffset            = currentVirtualAddress & (PAGE_SIZE - 1);
        const uint64_t chunkSize             = ((size - offset) < (PAGE_SIZE - pageOffset)) ? (size - offset) : (PAGE_SIZE - pageOffset);

        phys_addr_t physicalAddress = vmm_virt_to_phys(currentVirtualAddress, addressSpace);
        if (physicalAddress == 0)
        {
            return false;
        }

        uint8_t* destination = (uint8_t*) pa_to_hhdm((uintptr_t) physicalAddress, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
        if (destination == nullptr)
        {
            return false;
        }

        memcpy(destination + pageOffset, sourceBytes + offset, (size_t) chunkSize);
        offset += chunkSize;
    }

    return true;
}

static bool push_word_to_process_stack(virt_addr_space_t* addressSpace, uint64_t* writeCursor, uint64_t value)
{
    if (addressSpace == nullptr || writeCursor == nullptr)
    {
        return false;
    }

    if (!copy_to_process_virtual(addressSpace, *writeCursor, &value, sizeof(value)))
    {
        return false;
    }

    *writeCursor += sizeof(uint64_t);
    return true;
}
} // namespace

ProcessManager::ProcessManager()
{
    ManagerLock = 0;

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        RunningProcesses[i] = nullptr;
    }

    for (size_t i = 0; i < MAX_PROCESSES; i++)
    {
        Processes[i].allocated           = false;
        Processes[i].id                  = (uint64_t) i;
        Processes[i].hasParent           = false;
        Processes[i].parentId            = 0;
        Processes[i].exited              = false;
        Processes[i].exitStatus          = 0;
        Processes[i].addressSpace        = nullptr;
        Processes[i].elfMetadata         = nullptr;
        Processes[i].tasks               = nullptr;
        Processes[i].fileDescriptors     = nullptr;
        Processes[i].fileDescriptorCount = 0;
    }
}

void ProcessManager::LockManager() const
{
    spinlock_acquire((spinlock_t*) &ManagerLock);
}

void ProcessManager::UnlockManager() const
{
    spinlock_release((spinlock_t*) &ManagerLock);
}

process_t* ProcessManager::AllocateProcessUnlocked()
{
    for (size_t i = 0; i < MAX_PROCESSES; i++)
    {
        if (!Processes[i].allocated)
        {
            Processes[i].allocated           = true;
            Processes[i].id                  = (uint64_t) i;
            Processes[i].hasParent           = false;
            Processes[i].parentId            = 0;
            Processes[i].exited              = false;
            Processes[i].exitStatus          = 0;
            Processes[i].addressSpace        = nullptr;
            Processes[i].elfMetadata         = nullptr;
            Processes[i].tasks               = nullptr;
            Processes[i].fileDescriptors     = nullptr;
            Processes[i].fileDescriptorCount = 0;
            return &Processes[i];
        }
    }

    return nullptr;
}

process_t* ProcessManager::AllocateProcess()
{
    LockManager();
    process_t* process = AllocateProcessUnlocked();
    UnlockManager();
    return process;
}

process_t* ProcessManager::CreateProcess(virt_addr_space_t* addressSpace)
{
    if (addressSpace == nullptr)
    {
        return nullptr;
    }

    file_descriptor_t* fileDescriptorTable = (file_descriptor_t*) kmalloc(sizeof(file_descriptor_t) * DEFAULT_FILE_DESCRIPTOR_COUNT);
    if (fileDescriptorTable == nullptr)
    {
        return nullptr;
    }

    memset(fileDescriptorTable, 0, sizeof(file_descriptor_t) * DEFAULT_FILE_DESCRIPTOR_COUNT);

    LockManager();

    process_t* process = AllocateProcessUnlocked();
    if (process == nullptr)
    {
        UnlockManager();
        kfree(fileDescriptorTable);
        return nullptr;
    }

    process->fileDescriptors = fileDescriptorTable;
    process->fileDescriptorCount = DEFAULT_FILE_DESCRIPTOR_COUNT;
    process->addressSpace        = addressSpace;
    process->elfMetadata         = nullptr;
    process->exited              = false;
    process->exitStatus          = 0;

    const uint8_t currentCpu = arch_cpu_id();
    if (currentCpu < BOOT_SMP_MAX_CPUS && RunningProcesses[currentCpu] != nullptr)
    {
        process->hasParent = true;
        process->parentId  = RunningProcesses[currentCpu]->id;
    }
    else
    {
        process->hasParent = false;
        process->parentId  = 0;
    }

    UnlockManager();
    return process;
}

bool ProcessManager::FreeProcess(process_t* process)
{
    file_descriptor_t* descriptorsToFree = nullptr;

    LockManager();

    if (process == nullptr)
    {
        UnlockManager();
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        UnlockManager();
        return false;
    }

    if (!process->allocated)
    {
        UnlockManager();
        return false;
    }

    if (process->fileDescriptors != nullptr)
    {
        descriptorsToFree       = process->fileDescriptors;
        process->fileDescriptors = nullptr;
    }

    process->addressSpace        = nullptr;
    process->elfMetadata         = nullptr;
    process->id                  = (uint64_t) (process - &Processes[0]);
    process->hasParent           = false;
    process->parentId            = 0;
    process->exited              = false;
    process->exitStatus          = 0;
    process->allocated           = false;
    process->tasks               = nullptr;
    process->fileDescriptorCount = 0;

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        if (RunningProcesses[i] == process)
        {
            RunningProcesses[i] = nullptr;
        }
    }

    UnlockManager();

    if (descriptorsToFree != nullptr)
    {
        kfree(descriptorsToFree);
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
    uint64_t       newCount = oldCount * 2;
    if (newCount < oldCount)
    {
        return -1;
    }

    file_descriptor_t* newTable = (file_descriptor_t*) kmalloc(sizeof(file_descriptor_t) * newCount);
    if (newTable == nullptr)
    {
        return -1;
    }

    memcpy(newTable, process->fileDescriptors, sizeof(file_descriptor_t) * oldCount);

    memset(newTable + oldCount, 0, sizeof(file_descriptor_t) * (newCount - oldCount));

    kfree(process->fileDescriptors);
    process->fileDescriptors     = newTable;
    process->fileDescriptorCount = newCount;

    process->fileDescriptors[oldCount].file  = file;
    process->fileDescriptors[oldCount].flags = flags;
    return (int64_t) oldCount;
}

bool ProcessManager::BuildUserInitialStack(process_t* process, const process_user_stack_layout_t* layout, uint64_t* outUserRsp)
{
    if (process == nullptr || layout == nullptr || outUserRsp == nullptr)
    {
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return false;
    }

    if (!process->allocated || process->addressSpace == nullptr)
    {
        return false;
    }

    if (layout->stackSize == 0)
    {
        return false;
    }

    if (layout->argc > 0 && layout->argv == nullptr)
    {
        return false;
    }

    if (layout->envc > 0 && layout->envp == nullptr)
    {
        return false;
    }

    if (layout->auxvCount > 0 && layout->auxv == nullptr)
    {
        return false;
    }

    if (add_would_overflow_u64(layout->stackBase, layout->stackSize))
    {
        return false;
    }

    const uint64_t stackLimit        = layout->stackBase + layout->stackSize;
    uint64_t       stackCursor       = stackLimit;
    uint64_t       metadataWordCount = 0;
    uint64_t       metadataByteCount = 0;
    uint64_t       writeCursor       = 0;

    uint64_t* argvPointers = nullptr;
    uint64_t* envpPointers = nullptr;

    if (layout->argc > 0)
    {
        argvPointers = (uint64_t*) kmalloc(sizeof(uint64_t) * (size_t) layout->argc);
        if (argvPointers == nullptr)
        {
            return false;
        }
        memset(argvPointers, 0, sizeof(uint64_t) * (size_t) layout->argc);
    }

    if (layout->envc > 0)
    {
        envpPointers = (uint64_t*) kmalloc(sizeof(uint64_t) * (size_t) layout->envc);
        if (envpPointers == nullptr)
        {
            if (argvPointers != nullptr)
            {
                kfree(argvPointers);
            }
            return false;
        }
        memset(envpPointers, 0, sizeof(uint64_t) * (size_t) layout->envc);
    }

    for (uint64_t i = layout->argc; i > 0; --i)
    {
        const char* argumentString = layout->argv[i - 1];
        if (argumentString == nullptr)
        {
            goto fail;
        }

        const uint64_t argumentLength = (uint64_t) strlen(argumentString) + 1;
        if (stackCursor < layout->stackBase + argumentLength)
        {
            goto fail;
        }

        stackCursor -= argumentLength;
        if (!copy_to_process_virtual(process->addressSpace, stackCursor, argumentString, argumentLength))
        {
            goto fail;
        }

        argvPointers[i - 1] = stackCursor;
    }

    for (uint64_t i = layout->envc; i > 0; --i)
    {
        const char* environmentString = layout->envp[i - 1];
        if (environmentString == nullptr)
        {
            goto fail;
        }

        const uint64_t environmentLength = (uint64_t) strlen(environmentString) + 1;
        if (stackCursor < layout->stackBase + environmentLength)
        {
            goto fail;
        }

        stackCursor -= environmentLength;
        if (!copy_to_process_virtual(process->addressSpace, stackCursor, environmentString, environmentLength))
        {
            goto fail;
        }

        envpPointers[i - 1] = stackCursor;
    }

    stackCursor = align_down(stackCursor, 16);

    if (add_would_overflow_u64(layout->auxvCount, layout->argc) || add_would_overflow_u64(layout->auxvCount, layout->envc))
    {
        goto fail;
    }

    metadataWordCount = 1 + (layout->argc + 1) + (layout->envc + 1) + (layout->auxvCount * 2);
    metadataByteCount = metadataWordCount * sizeof(uint64_t);

    if (stackCursor < layout->stackBase + metadataByteCount)
    {
        goto fail;
    }

    stackCursor -= metadataByteCount;
    writeCursor = stackCursor;

    if (!push_word_to_process_stack(process->addressSpace, &writeCursor, layout->argc))
    {
        goto fail;
    }

    for (uint64_t i = 0; i < layout->argc; ++i)
    {
        if (!push_word_to_process_stack(process->addressSpace, &writeCursor, argvPointers[i]))
        {
            goto fail;
        }
    }

    if (!push_word_to_process_stack(process->addressSpace, &writeCursor, 0))
    {
        goto fail;
    }

    for (uint64_t i = 0; i < layout->envc; ++i)
    {
        if (!push_word_to_process_stack(process->addressSpace, &writeCursor, envpPointers[i]))
        {
            goto fail;
        }
    }

    if (!push_word_to_process_stack(process->addressSpace, &writeCursor, 0))
    {
        goto fail;
    }

    for (uint64_t i = 0; i < layout->auxvCount; ++i)
    {
        if (!push_word_to_process_stack(process->addressSpace, &writeCursor, layout->auxv[i].type) || !push_word_to_process_stack(process->addressSpace, &writeCursor, layout->auxv[i].value))
        {
            goto fail;
        }
    }

    *outUserRsp = stackCursor;

    if (argvPointers != nullptr)
    {
        kfree(argvPointers);
    }

    if (envpPointers != nullptr)
    {
        kfree(envpPointers);
    }
    return true;

fail:
    if (argvPointers != nullptr)
    {
        kfree(argvPointers);
    }

    if (envpPointers != nullptr)
    {
        kfree(envpPointers);
    }
    return false;
}

bool ProcessManager::ActivateProcessAddressSpace(process_t* process)
{
    virt_addr_space_t* targetAddressSpace = nullptr;

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
        targetAddressSpace      = process->addressSpace;
        platform.cpus[cpuId].address_space = targetAddressSpace;
        vmm_switch_addr_space(targetAddressSpace);
        return true;
    }

    if (current == process)
    {
        platform.cpus[cpuId].address_space = process->addressSpace;
        return true;
    }

    RunningProcesses[cpuId] = process;
    targetAddressSpace      = process->addressSpace;
    platform.cpus[cpuId].address_space = targetAddressSpace;
    vmm_switch_addr_space(targetAddressSpace);
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
    bool               shouldSwitch      = false;
    virt_addr_space_t* targetAddressSpace = nullptr;

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

    if (process != nullptr && process->addressSpace != nullptr && cpuId == arch_cpu_id())
    {
        shouldSwitch       = true;
        targetAddressSpace = process->addressSpace;
        platform.cpus[cpuId].address_space = targetAddressSpace;
    }

    if (shouldSwitch)
    {
        vmm_switch_addr_space(targetAddressSpace);
    }

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

bool ProcessManager::IsProcessRunning(const process_t* process) const
{
    if (process == nullptr)
    {
        return false;
    }

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; ++i)
    {
        if (RunningProcesses[i] == process)
        {
            return true;
        }
    }

    return false;
}

bool ProcessManager::TryReapExitedProcess(uint64_t processId)
{
    if (processId >= MAX_PROCESSES)
    {
        return false;
    }

    process_t* process = &Processes[processId];
    if (!process->allocated)
    {
        return false;
    }

    if (!process->exited)
    {
        return false;
    }

    if (IsProcessRunning(process))
    {
        return false;
    }

    return FreeProcess(process);
}

size_t ProcessManager::GetCapacity() const
{
    return MAX_PROCESSES;
}
