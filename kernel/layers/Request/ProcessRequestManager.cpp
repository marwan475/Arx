#include "layers/Request/ProcessRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/ProcessController.hpp"
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

ProcessRequestManager::ProcessRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps) : ResourceCaps(resourceLayerCaps), LogicCaps(logicLayerCaps)
{
}

uint64_t ProcessRequestManager::HandleExitRequest(const arch_syscall_frame_t* frame)
{
    return LogicCaps != nullptr && LogicCaps->processController != nullptr ? LogicCaps->processController->Exit(frame != nullptr ? static_cast<int32_t>(frame->arg0) : 0, false) : LINUX_ENOSYS;
}

uint64_t ProcessRequestManager::HandleExit_groupRequest(const arch_syscall_frame_t* frame)
{
    return LogicCaps != nullptr && LogicCaps->processController != nullptr ? LogicCaps->processController->Exit(frame != nullptr ? static_cast<int32_t>(frame->arg0) : 0, true) : LINUX_ENOSYS;
}

uint64_t ProcessRequestManager::HandleForkRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_clone_arguments_t arguments = {};
    arguments.exitSignal               = 17;
    return LogicCaps->processController->Clone(frame, arguments);
}

uint64_t ProcessRequestManager::HandleVforkRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_clone_arguments_t arguments = {};
    arguments.flags                    = 0x4100;
    arguments.exitSignal               = 17;
    return LogicCaps->processController->Clone(frame, arguments);
}

uint64_t ProcessRequestManager::HandleCloneRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    process_clone_arguments_t arguments = {};
    arguments.flags                    = frame->arg0 & ~255ULL;
    arguments.exitSignal               = frame->arg0 & 255;
    arguments.stack                    = frame->arg1;
    arguments.parentTid                = frame->arg2;
    arguments.childTid                 = frame->arg3;
    arguments.tls                      = frame->arg4;
    return LogicCaps->processController->Clone(frame, arguments);
}

uint64_t ProcessRequestManager::HandleClone3Request(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if (frame == nullptr || frame->arg1 < 64)
    {
        return LINUX_EINVAL;
    }

    if (frame->arg1 > PAGE_SIZE)
    {
        return static_cast<uint64_t>(-7);
    }

    process_clone_arguments_t arguments = {};
    const size_t size                  = frame->arg1 < sizeof(arguments) ? frame->arg1 : sizeof(arguments);

    if (!safe_read_user(frame->arg0, &arguments, size))
    {
        return LINUX_EFAULT;
    }

    for (uint64_t offset = sizeof(arguments); offset < frame->arg1; ++offset)
    {
        uint8_t extra;

        if (frame->arg0 > UINT64_MAX - offset || !safe_read_user(frame->arg0 + offset, &extra, 1))
        {
            return LINUX_EFAULT;
        }

        if (extra != 0)
        {
            return static_cast<uint64_t>(-7);
        }
    }

    if ((arguments.flags & 255) != 0 || (arguments.stack == 0) != (arguments.stackSize == 0) || arguments.stackSize > CANONICAL_USER_END || arguments.stack > CANONICAL_USER_END - arguments.stackSize)
    {
        return LINUX_EINVAL;
    }

    arguments.stack += arguments.stackSize;
    return LogicCaps->processController->Clone(frame, arguments);
}

uint64_t ProcessRequestManager::HandleExecveRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return LogicCaps->processController->Execute(const_cast<arch_syscall_frame_t*>(frame));
}

uint64_t ProcessRequestManager::HandleWait4Request(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return frame != nullptr ? LogicCaps->processController->Wait(static_cast<int32_t>(frame->arg0), frame->arg1, static_cast<uint32_t>(frame->arg2), frame->arg3, false) : LINUX_EINVAL;
}

uint64_t ProcessRequestManager::HandleWaitidRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return frame != nullptr ? LogicCaps->processController->Wait(static_cast<uint32_t>(frame->arg1), frame->arg2, static_cast<uint32_t>(frame->arg3), frame->arg4, true, static_cast<uint32_t>(frame->arg0)) : LINUX_EINVAL;
}

uint64_t ProcessRequestManager::HandleSet_tid_addressRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return LogicCaps->processController->SetTidAddress(frame != nullptr ? frame->arg0 : 0);
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
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return LogicCaps->processController->Group(0, frame != nullptr ? static_cast<int32_t>(frame->arg0) : 0);
}

uint64_t ProcessRequestManager::HandleGetpgrpRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    (void) frame;
    return LogicCaps->processController->Group(0, 0);
}

uint64_t ProcessRequestManager::HandleSetpgidRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return frame != nullptr ? LogicCaps->processController->Group(1, static_cast<int32_t>(frame->arg0), static_cast<int32_t>(frame->arg1)) : LINUX_EINVAL;
}

uint64_t ProcessRequestManager::HandleGetsidRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return LogicCaps->processController->Group(2, frame != nullptr ? static_cast<int32_t>(frame->arg0) : 0);
}

uint64_t ProcessRequestManager::HandleSetsidRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    (void) frame;
    return LogicCaps->processController->Group(3, 0);
}
