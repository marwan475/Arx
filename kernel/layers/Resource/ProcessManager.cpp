#include "layers/Resource/ProcessManager.hpp"

#include "layers/Resource/VirtualMemoryManager.hpp"

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

process_t* FileDescriptorGuard::Active[BOOT_SMP_MAX_CPUS] = {};

FileDescriptorGuard::FileDescriptorGuard(process_t* process) : Process(process), Previous(nullptr), Owner(false)
{
    cpu_preemption_disable();
    const uint8_t cpu = arch_cpu_id();
    Previous = Active[cpu];
    Owner = process != nullptr && Previous != process;
    if (Owner)
        spinlock_acquire(&process->fileDescriptorLock);
    if (process != nullptr)
        Active[cpu] = process;
}

FileDescriptorGuard::~FileDescriptorGuard()
{
    if (Process != nullptr)
        Active[arch_cpu_id()] = Previous;
    if (Owner)
        spinlock_release(&Process->fileDescriptorLock);
    cpu_preemption_enable();
}

void ProcessManager::ResetProcessSlot(process_t* process, uint64_t id, bool allocated)
{
    process->allocated    = allocated;
    process->id           = id;
    process->hasParent    = false;
    process->parentId     = 0;
    process->exited       = false;
    process->reclaiming   = false;
    process->ownerCpu     = -1;
    process->exitStatus   = 0;
    process->addressSpace = nullptr;
    process->elfMetadata  = nullptr;
    process->breakBase    = 0;
    process->programBreak = 0;
    process->memoryUsers  = 0;
    memset(process->timerIntervalSeconds, 0, sizeof(process->timerIntervalSeconds));
    memset(process->timerIntervalMicroseconds, 0, sizeof(process->timerIntervalMicroseconds));
    process->tasks               = nullptr;
    process->fileDescriptors     = nullptr;
    process->fileDescriptorLock  = 0;
    process->fileDescriptorCount = 0;
    process->umask               = 0022U;
    memset(process->cwdPath, 0, sizeof(process->cwdPath));
    process->cwdPath[0] = '/';
}

ProcessManager::ProcessManager()
{
    ManagerLock = 0;

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        RunningProcesses[i] = nullptr;
    }

    for (size_t i = 0; i < MAX_PROCESSES; i++)
    {
        ResetProcessSlot(&Processes[i], i, false);
    }
}

void ProcessManager::LockManager() const
{
    cpu_preemption_disable();
    spinlock_acquire((spinlock_t*) &ManagerLock);
}

void ProcessManager::UnlockManager() const
{
    spinlock_release((spinlock_t*) &ManagerLock);
    cpu_preemption_enable();
}

process_t* ProcessManager::AllocateProcessUnlocked()
{
    for (size_t i = 0; i < MAX_PROCESSES; i++)
    {
        if (!Processes[i].allocated)
        {
            ResetProcessSlot(&Processes[i], i, true);
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

    // Shared roots currently have no per-process ownership model. Do not attach
    // another process once syscall-owned pages exist in this address space.
    for (size_t i = 0; i < MAX_PROCESSES; ++i)
    {
        if (Processes[i].allocated && Processes[i].addressSpace == addressSpace && Processes[i].memoryUsers != 0)
        {
            UnlockManager();
            kfree(fileDescriptorTable);
            return nullptr;
        }
    }
    spinlock_acquire(&addressSpace->operation_lock);
    bool hasAnonymousPages = false;
    for (virt_region_t* r = addressSpace->user_used_regions; r; r = r->next)
        if (r->backing)
        {
            hasAnonymousPages = true;
            break;
        }
    spinlock_release(&addressSpace->operation_lock);
    if (hasAnonymousPages)
    {
        UnlockManager();
        kfree(fileDescriptorTable);
        return nullptr;
    }

    process_t* process = AllocateProcessUnlocked();
    if (process == nullptr)
    {
        UnlockManager();
        kfree(fileDescriptorTable);
        return nullptr;
    }

    process->fileDescriptors     = fileDescriptorTable;
    process->fileDescriptorCount = DEFAULT_FILE_DESCRIPTOR_COUNT;
    process->addressSpace        = addressSpace;
    process->elfMetadata         = nullptr;
    process->exited              = false;
    process->exitStatus          = 0;
    memset(process->cwdPath, 0, sizeof(process->cwdPath));
    process->cwdPath[0] = '/';

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

bool ProcessManager::FreeProcess(process_t* process, bool onlyIfExited)
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

    if (!process->allocated || (onlyIfExited && !process->exited) || process->memoryUsers || IsProcessRunning(process))
    {
        UnlockManager();
        return false;
    }
    for (task_t* task = process->tasks; task != nullptr; task = task->next)
    {
        if (task->allocated)
        {
            UnlockManager();
            return false;
        }
    }

    // Pin the slot and reject new memory operations while teardown runs.
    // Do not retain ManagerLock across page-table operations and IPI waits.
    process->memoryUsers            = 1;
    process->reclaiming             = true;
    const bool wasExited            = process->exited;
    process->exited                 = true;
    virt_addr_space_t* addressSpace = process->addressSpace;
    UnlockManager();
    VirtualMemoryManager memory;
    memory.ReleaseAnonymousPages(addressSpace);
    LockManager();
    process->memoryUsers = 0;
    process->exited      = wasExited;

    if (process->fileDescriptors != nullptr)
    {
        descriptorsToFree        = process->fileDescriptors;
        process->fileDescriptors = nullptr;
    }

    ResetProcessSlot(process, (uint64_t) (process - &Processes[0]), false);

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
    return AddFileDescriptorFrom(process, file, flags, 0);
}

file_descriptor_t* ProcessManager::GetFileDescriptor(process_t* process, uint64_t fd)
{
    if (process == nullptr || process->fileDescriptors == nullptr || fd >= process->fileDescriptorCount)
        return nullptr;
    file_descriptor_t* descriptor = &process->fileDescriptors[fd];
    return descriptor->file != nullptr ? descriptor : nullptr;
}

bool ProcessManager::EnsureFileDescriptorCapacity(process_t* process, uint64_t requiredIndex)
{
    FileDescriptorGuard descriptors(process);
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

    if (requiredIndex < process->fileDescriptorCount && process->fileDescriptors != nullptr)
    {
        return true;
    }

    uint64_t oldCount = process->fileDescriptorCount;
    uint64_t newCount = oldCount;

    if (newCount == 0)
    {
        newCount = DEFAULT_FILE_DESCRIPTOR_COUNT;
        oldCount = 0;
    }

    while (requiredIndex >= newCount)
    {
        if (newCount > (UINT64_MAX / 2ULL))
        {
            return false;
        }
        newCount *= 2ULL;
    }

    if (newCount > SIZE_MAX / sizeof(file_descriptor_t))
        return false;
    file_descriptor_t* newTable = (file_descriptor_t*) kmalloc(sizeof(file_descriptor_t) * newCount);
    if (newTable == nullptr)
    {
        return false;
    }

    memset(newTable, 0, sizeof(file_descriptor_t) * newCount);

    if (process->fileDescriptors != nullptr && process->fileDescriptorCount > 0)
    {
        memcpy(newTable, process->fileDescriptors, sizeof(file_descriptor_t) * process->fileDescriptorCount);
        kfree(process->fileDescriptors);
    }

    process->fileDescriptors     = newTable;
    process->fileDescriptorCount = newCount;
    return true;
}

int64_t ProcessManager::AddFileDescriptorFrom(process_t* process, file_handle_t file, uint32_t flags, uint64_t minFd)
{
    FileDescriptorGuard descriptors(process);
    if (process == nullptr || file == nullptr)
    {
        return -1;
    }

    if (!EnsureFileDescriptorCapacity(process, minFd))
    {
        return -1;
    }

    for (uint64_t i = minFd; i < process->fileDescriptorCount; i++)
    {
        if (process->fileDescriptors[i].file == nullptr)
        {
            process->fileDescriptors[i].file  = file;
            process->fileDescriptors[i].flags = flags;
            return (int64_t) i;
        }
    }

    const uint64_t growFrom = process->fileDescriptorCount;
    if (!EnsureFileDescriptorCapacity(process, growFrom))
    {
        return -1;
    }

    for (uint64_t i = minFd; i < process->fileDescriptorCount; i++)
    {
        if (process->fileDescriptors[i].file == nullptr)
        {
            process->fileDescriptors[i].file  = file;
            process->fileDescriptors[i].flags = flags;
            return (int64_t) i;
        }
    }

    return -1;
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

    if (add_overflows_u64(layout->stackBase, layout->stackSize))
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

    if (add_overflows_u64(layout->auxvCount, layout->argc) || add_overflows_u64(layout->auxvCount, layout->envc))
    {
        goto fail;
    }

    metadataWordCount = 1 + (layout->argc + 1) + (layout->envc + 1) + (layout->auxvCount * 2);
    metadataByteCount = metadataWordCount * sizeof(uint64_t);

    if (stackCursor < layout->stackBase + metadataByteCount)
    {
        goto fail;
    }

    stackCursor = align_down(stackCursor - metadataByteCount, 16);
    if (stackCursor < layout->stackBase)
    {
        goto fail;
    }
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
    return process != nullptr && process->addressSpace != nullptr && SetRunningProcess(arch_cpu_id(), process);
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

    return __atomic_load_n(&RunningProcesses[cpuId], __ATOMIC_ACQUIRE);
}

process_t* ProcessManager::GetCurrentProcess() const
{
    return GetRunningProcess(arch_cpu_id());
}

bool ProcessManager::SetRunningProcess(uint8_t cpuId, process_t* process)
{
    bool               shouldSwitch       = false;
    virt_addr_space_t* targetAddressSpace = nullptr;

    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    LockManager();
    if (process != nullptr)
    {
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

        if (process->reclaiming || (process->ownerCpu >= 0 && process->ownerCpu != cpuId))
        {
            UnlockManager();
            return false;
        }
        process->ownerCpu = cpuId;
    }

    // Claim execution under the same manager lock used by teardown.
    for (size_t other = 0; process != nullptr && other < BOOT_SMP_MAX_CPUS; ++other)
    {
        if (other != cpuId && RunningProcesses[other] == process)
        {
            UnlockManager();
            return false;
        }
    }
    __atomic_store_n(&RunningProcesses[cpuId], process, __ATOMIC_RELEASE);

    if (process != nullptr && process->addressSpace != nullptr && cpuId == arch_cpu_id())
    {
        shouldSwitch                       = true;
        targetAddressSpace                 = process->addressSpace;
        platform.cpus[cpuId].address_space = targetAddressSpace;
    }

    if (shouldSwitch)
    {
        vmm_switch_addr_space(targetAddressSpace);
    }

    UnlockManager();
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
        if (__atomic_load_n(&RunningProcesses[i], __ATOMIC_ACQUIRE) == process)
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

    return FreeProcess(process, true);
}

size_t ProcessManager::GetCapacity() const
{
    return MAX_PROCESSES;
}

process_t* ProcessManager::AcquireCurrentMemoryProcess(bool requireExclusive)
{
    LockManager();
    process_t* process = GetCurrentProcess();
    if (process && process->allocated && !process->exited && process->addressSpace)
    {
        for (size_t i = 0; requireExclusive && i < MAX_PROCESSES; ++i)
            if (&Processes[i] != process && Processes[i].allocated && Processes[i].addressSpace == process->addressSpace)
            {
                process = nullptr;
                break;
            }
        if (process)
            process->memoryUsers++;
    }
    else
        process = nullptr;
    UnlockManager();
    return process;
}
void ProcessManager::ReleaseMemoryProcess(process_t* process)
{
    LockManager();
    if (process && process->memoryUsers)
        process->memoryUsers--;
    UnlockManager();
}

uint64_t ProcessManager::GetCurrentProgramBreak() const
{
    LockManager();
    process_t*     process = GetCurrentProcess();
    const uint64_t result  = process != nullptr && process->allocated ? process->programBreak : 0;
    UnlockManager();
    return result;
}
