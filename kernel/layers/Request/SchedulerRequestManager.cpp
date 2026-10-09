#include "layers/Request/SchedulerRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

extern "C"
{
#include <arch/arch.h>
#include <boot/boot.h>
#include <klib/klib.h>
#include <platform.h>
}

SchedulerRequestManager::SchedulerRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps)
    : ResourceCaps(resourceLayerCaps), LogicCaps(logicLayerCaps)
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
    (void) LogicCaps->scheduler->RunNextReadyProcess(cpuId);
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

    const int64_t pid = (int64_t) frame->arg0;
    if (pid != 0 && (uint64_t) pid != currentProcess->id)
    {
        return LINUX_ESRCH;
    }

    const uint64_t setSize = frame->arg1;
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

    uint64_t mask = 0;
    const uint8_t cpuCount = (uint8_t) ((platform.cpu_count > BOOT_SMP_MAX_CPUS) ? BOOT_SMP_MAX_CPUS : platform.cpu_count);
    for (uint8_t cpu = 0; cpu < cpuCount; ++cpu)
    {
        mask |= (1ULL << cpu);
    }

    memcpy(affinityMaskBuffer, &mask, sizeof(mask));
    if (!ResourceCaps->virtualMemoryManager->CopyToUser(userSet, affinityMaskBuffer, sizeof(affinityMaskBuffer), currentProcess->addressSpace))
    {
        return LINUX_EFAULT;
    }

    if (setSize > requiredSetSize)
    {
        constexpr uint8_t zeroBuffer[16] = {0};
        uint64_t remaining = setSize - requiredSetSize;
        uintptr_t current  = userSet + requiredSetSize;
        while (remaining > 0)
        {
            const uint64_t chunk = remaining > sizeof(zeroBuffer) ? sizeof(zeroBuffer) : remaining;
            if (!ResourceCaps->virtualMemoryManager->CopyToUser(current, zeroBuffer, (size_t) chunk, currentProcess->addressSpace))
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

    if (cpuPtr != 0)
    {
        const uint32_t cpuId = (uint32_t) arch_cpu_id();
        if (!ResourceCaps->virtualMemoryManager->CopyToUser(cpuPtr, &cpuId, sizeof(cpuId), currentProcess->addressSpace))
        {
            return LINUX_EFAULT;
        }
    }

    if (nodePtr != 0)
    {
        const uint32_t nodeId = 0;
        if (!ResourceCaps->virtualMemoryManager->CopyToUser(nodePtr, &nodeId, sizeof(nodeId), currentProcess->addressSpace))
        {
            return LINUX_EFAULT;
        }
    }

    return 0;
}
