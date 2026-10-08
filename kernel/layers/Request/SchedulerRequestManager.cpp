#include "layers/Request/SchedulerRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

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

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr)
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
    uint8_t*       userSet = (uint8_t*) (uintptr_t) frame->arg2;
    if (userSet == nullptr)
    {
        return LINUX_EFAULT;
    }

    constexpr uint64_t requiredSetSize = sizeof(uint64_t);
    if (setSize < requiredSetSize)
    {
        return LINUX_EINVAL;
    }

    memset(userSet, 0, (size_t) setSize);

    uint64_t mask = 0;
    const uint8_t cpuCount = (uint8_t) ((platform.cpu_count > BOOT_SMP_MAX_CPUS) ? BOOT_SMP_MAX_CPUS : platform.cpu_count);
    for (uint8_t cpu = 0; cpu < cpuCount; ++cpu)
    {
        mask |= (1ULL << cpu);
    }

    memcpy(userSet, &mask, sizeof(mask));
    return requiredSetSize;
}

uint64_t SchedulerRequestManager::HandleGetcpuRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    uint32_t* cpuPtr  = (uint32_t*) (uintptr_t) frame->arg0;
    uint32_t* nodePtr = (uint32_t*) (uintptr_t) frame->arg1;

    if (cpuPtr != nullptr)
    {
        *cpuPtr = (uint32_t) arch_cpu_id();
    }

    if (nodePtr != nullptr)
    {
        *nodePtr = 0;
    }

    return 0;
}
