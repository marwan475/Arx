#pragma once

#include <arch/arch.h>
#include <stdint.h>

class TimeRequestManager
{
public:
    uint64_t HandleTimeRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGettimeofdayRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleClock_gettimeRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleClock_getresRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleClock_nanosleepRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleNanosleepRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetitimerRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetitimerRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleAlarmRequest(const arch_syscall_frame_t* frame);
};
