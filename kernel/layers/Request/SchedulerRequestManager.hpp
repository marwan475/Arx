#pragma once

#include <arch/arch.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct LogicLayerCaps;

class SchedulerRequestManager
{
public:
    SchedulerRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps);

    uint64_t HandleSched_yieldRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSched_getaffinityRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetcpuRequest(const arch_syscall_frame_t* frame);

private:
    ResourceLayerCaps* ResourceCaps;
    LogicLayerCaps*    LogicCaps;
};
