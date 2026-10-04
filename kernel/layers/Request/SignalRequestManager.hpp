#pragma once

#include <arch/arch.h>
#include <stdint.h>

class SignalRequestManager
{
public:
    uint64_t Handlert_sigreturnRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlert_sigprocmaskRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlert_sigactionRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesigaltstackRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlekillRequest(const arch_syscall_frame_t* frame);
    uint64_t HandletgkillRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlepauseRequest(const arch_syscall_frame_t* frame);
};
