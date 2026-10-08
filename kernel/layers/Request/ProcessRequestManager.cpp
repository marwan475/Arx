#include "layers/Request/ProcessRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <arch/arch.h>
}

namespace
{
static void close_process_file_descriptors(process_t* process, VirtualFileSystem* virtualFileSystem)
{
    if (process == nullptr || virtualFileSystem == nullptr)
    {
        return;
    }

    if (process->fileDescriptors == nullptr || process->fileDescriptorCount == 0)
    {
        return;
    }

    for (uint64_t fd = 0; fd < process->fileDescriptorCount; ++fd)
    {
        file_descriptor_t* descriptor = &process->fileDescriptors[fd];
        if (descriptor->file == nullptr)
        {
            continue;
        }

        file_t* file      = static_cast<file_t*>(descriptor->file);
        descriptor->file  = nullptr;
        descriptor->flags = FD_FLAG_NONE;
        (void) virtualFileSystem->Close(file);
    }
}
} // namespace

ProcessRequestManager::ProcessRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps)
    : ResourceCaps(resourceLayerCaps), LogicCaps(logicLayerCaps)
{
}

uint64_t ProcessRequestManager::HandleExitRequest(const arch_syscall_frame_t* frame)
{
    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->taskManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    task_t*    currentTask    = ResourceCaps->taskManager->GetCurrentTask();
    if (currentProcess == nullptr || currentTask == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int32_t status  = frame != nullptr ? (int32_t) frame->arg0 : 0;

    if (LogicCaps != nullptr && LogicCaps->virtualFileSystem != nullptr)
    {
        close_process_file_descriptors(currentProcess, LogicCaps->virtualFileSystem);
    }

    if (LogicCaps != nullptr && LogicCaps->scheduler != nullptr)
    {
        if (LogicCaps->scheduler->KillProcess(currentProcess->id, status))
        {
            return 0;
        }
    }

    currentProcess->exited     = true;
    currentProcess->exitStatus = status;

    // For kernel/selftest tasks, keep legacy returnable behavior so in-kernel
    // test harnesses can continue executing.
    if (!currentTask->isUserTask)
    {
        return 0;
    }

    // User-task exit should not return to userspace.
    if (LogicCaps != nullptr && LogicCaps->scheduler != nullptr)
    {
        const uint8_t cpuId = arch_cpu_id();
        while (LogicCaps->scheduler->RunNextReadyProcess(cpuId))
        {
        }
    }

    for (;;)
    {
        arch_pause();
    }

    return 0;
}

uint64_t ProcessRequestManager::HandleExit_groupRequest(const arch_syscall_frame_t* frame)
{
    return HandleExitRequest(frame);
}

uint64_t ProcessRequestManager::HandleForkRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleVforkRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleCloneRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleClone3Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleExecveRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleWait4Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleWaitidRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleSet_tid_addressRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleGettidRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;

    if (ResourceCaps == nullptr || ResourceCaps->taskManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    task_t* currentTask = ResourceCaps->taskManager->GetCurrentTask();
    if (currentTask == nullptr)
    {
        return LINUX_ESRCH;
    }

    return currentTask->id;
}

uint64_t ProcessRequestManager::HandleGetpidRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    return currentProcess->id;
}

uint64_t ProcessRequestManager::HandleGetppidRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    if (!currentProcess->hasParent)
    {
        return 0;
    }

    return currentProcess->parentId;
}

uint64_t ProcessRequestManager::HandleGetpgidRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleGetpgrpRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleSetpgidRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleGetsidRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleSetsidRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    (void) LogicCaps;
    return (uint64_t) -38;
}
