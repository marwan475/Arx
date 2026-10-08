#include "layers/Request/SchedulerRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Request/RequestLayerFactory.hpp"

extern "C"
{
#include <arch/arch.h>
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
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SchedulerRequestManager::HandleGetcpuRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}
