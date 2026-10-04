#pragma once

#include <arch/arch.h>
#include <stdint.h>

class TimeRequestManager
{
public:
    uint64_t HandletimeRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegettimeofdayRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleclock_gettimeRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleclock_getresRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleclock_nanosleepRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlenanosleepRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetitimerRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetitimerRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlealarmRequest(const arch_syscall_frame_t* frame);
};
