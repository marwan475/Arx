#include "layers/Request/ProcessRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

namespace
{
constexpr uint64_t LINUX_ENOSYS = (uint64_t) -38;
constexpr uint64_t LINUX_ESRCH  = (uint64_t) -3;
}

ProcessRequestManager::ProcessRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps)
    : ResourceCaps(resourceLayerCaps), LogicCaps(logicLayerCaps)
{
}

uint64_t ProcessRequestManager::HandleExitRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t ProcessRequestManager::HandleExit_groupRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
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
    return (uint64_t) -38;
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
