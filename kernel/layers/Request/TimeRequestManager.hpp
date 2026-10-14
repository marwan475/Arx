#pragma once

#include <arch/arch.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct LogicLayerCaps;
class TimeManager;

class TimeRequestManager
{
public:
    explicit TimeRequestManager(ResourceLayerCaps* resourceLayerCaps = nullptr, LogicLayerCaps* logicLayerCaps = nullptr) : ResourceCaps(resourceLayerCaps), LogicCaps(logicLayerCaps)
    {
    }
    uint64_t HandleTimeRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGettimeofdayRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleClock_gettimeRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleClock_getresRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleClock_nanosleepRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleNanosleepRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetitimerRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetitimerRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleAlarmRequest(const arch_syscall_frame_t* frame);

private:
    TimeManager*       GetTimeManager() const;
    uint64_t           SleepRequest(int clock, bool absolute, uint64_t requestAddress, uint64_t remainderAddress);
    ResourceLayerCaps* ResourceCaps;
    LogicLayerCaps*    LogicCaps;
};
