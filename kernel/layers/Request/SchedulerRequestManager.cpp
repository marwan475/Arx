#include "layers/Request/SchedulerRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/SynchronizationManager.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

extern "C"
{
#include <arch/arch.h>
#include <boot/boot.h>
#include <klib/klib.h>
#include <platform.h>
}

SchedulerRequestManager::SchedulerRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps) : ResourceCaps(resourceLayerCaps), LogicCaps(logicLayerCaps)
{
}

uint64_t SchedulerRequestManager::HandleSched_yieldRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;

    if (LogicCaps == nullptr || LogicCaps->scheduler == nullptr)
    {
        return LINUX_ENOSYS;
    }

    const uint8_t cpuId = arch_cpu_id();
    (void) LogicCaps->scheduler->RunNextReadyProcess(cpuId, Scheduler::scheduling_reason_t::YIELD);
    return 0;
}

uint64_t SchedulerRequestManager::HandleSched_getaffinityRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    if (ResourceCaps->taskManager == nullptr)
    {
        return LINUX_ENOSYS;
    }
    const int64_t tid  = (int64_t) frame->arg0;
    task_t*       task = tid == 0 ? ResourceCaps->taskManager->GetCurrentTask() : tid > 0 ? ResourceCaps->taskManager->FindTask(tid) : nullptr;
    if (!ResourceCaps->taskManager->RetainForDispatch(task))
    {
        return LINUX_ESRCH;
    }
    const uint64_t storedMask = __atomic_load_n(&task->affinityMask, __ATOMIC_ACQUIRE);
    ResourceCaps->taskManager->ReleaseDispatchClaim(task);

    const uint64_t  setSize = frame->arg1;
    const uintptr_t userSet = (uintptr_t) frame->arg2;

    if (userSet == 0)
    {
        return LINUX_EFAULT;
    }

    constexpr uint64_t requiredSetSize = sizeof(uint64_t);

    if (setSize < requiredSetSize)
    {
        return LINUX_EINVAL;
    }

    uint8_t affinityMaskBuffer[sizeof(uint64_t)] = {0};

    const uint64_t mask = storedMask;

    memcpy(affinityMaskBuffer, &mask, sizeof(mask));

    if (!safe_write_user(userSet, affinityMaskBuffer, sizeof(affinityMaskBuffer)))
    {
        return LINUX_EFAULT;
    }

    if (setSize > requiredSetSize)
    {
        constexpr uint8_t zeroBuffer[16] = {0};
        uint64_t          remaining      = setSize - requiredSetSize;
        uintptr_t         current        = userSet + requiredSetSize;

        while (remaining > 0)
        {
            const uint64_t chunk = remaining > sizeof(zeroBuffer) ? sizeof(zeroBuffer) : remaining;

            if (!safe_write_user(current, zeroBuffer, (size_t) chunk))
            {
                return LINUX_EFAULT;
            }

            current += chunk;
            remaining -= chunk;
        }
    }

    return requiredSetSize;
}

uint64_t SchedulerRequestManager::HandleGetcpuRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uintptr_t cpuPtr  = (uintptr_t) frame->arg0;
    const uintptr_t nodePtr = (uintptr_t) frame->arg1;

    if (LogicCaps == nullptr || LogicCaps->scheduler == nullptr)
    {
        return LINUX_ENOSYS;
    }
    uint32_t       cpuId  = 0;
    uint32_t       nodeId = 0;
    const uint64_t status = LogicCaps->scheduler->GetCpu(&cpuId, nodePtr != 0 ? &nodeId : nullptr);
    if (status != 0)
    {
        return status;
    }
    if ((cpuPtr != 0 && !safe_write_user(cpuPtr, &cpuId, sizeof(cpuId))) || (nodePtr != 0 && !safe_write_user(nodePtr, &nodeId, sizeof(nodeId))))
    {
        return LINUX_EFAULT;
    }

    return 0;
}

uint64_t SchedulerRequestManager::HandleSched_setaffinityRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }
    if (LogicCaps == nullptr || LogicCaps->scheduler == nullptr)
    {
        return LINUX_ENOSYS;
    }
    if (frame->arg1 == 0)
    {
        return LINUX_EINVAL;
    }
    uint64_t     mask = 0;
    const size_t size = frame->arg1 < sizeof(mask) ? frame->arg1 : sizeof(mask);
    if (!safe_read_user(frame->arg2, &mask, size))
    {
        return LINUX_EFAULT;
    }
    task_t*        task   = nullptr;
    const uint64_t status = LogicCaps->scheduler->AcquireTask((int64_t) frame->arg0, true, &task);
    if (status != 0)
    {
        return status;
    }
    const uint64_t result = LogicCaps->scheduler->SetAffinity(task, mask);
    ResourceCaps->taskManager->ReleaseDispatchClaim(task);
    return result;
}

uint64_t SchedulerRequestManager::HandleGetpriorityRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || frame->arg0 > 2)
    {
        return LINUX_EINVAL;
    }
    if (LogicCaps == nullptr || LogicCaps->scheduler == nullptr)
    {
        return LINUX_ENOSYS;
    }
    return LogicCaps->scheduler->UserPriority(frame->arg0, frame->arg1, false, 0);
}

uint64_t SchedulerRequestManager::HandleSetpriorityRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || frame->arg0 > 2)
    {
        return LINUX_EINVAL;
    }
    if (LogicCaps == nullptr || LogicCaps->scheduler == nullptr)
    {
        return LINUX_ENOSYS;
    }
    const int32_t requested = static_cast<int32_t>(frame->arg2);
    const int32_t nice = requested < -20 ? -20 : requested > 19 ? 19 : requested;
    return LogicCaps->scheduler->UserPriority(frame->arg0, frame->arg1, true, nice);
}

void SchedulerRequestManager::HandleAccountMode(LogicLayerCaps* logic, bool userMode)
{

    if (logic != nullptr && logic->scheduler != nullptr)
    {
        logic->scheduler->AccountCurrent(userMode);
    }
}

void SchedulerRequestManager::HandlePendingReschedule(LogicLayerCaps* logic)
{
    if (logic != nullptr && logic->scheduler != nullptr)
    {
        logic->scheduler->ServicePendingReschedule();
    }
}
