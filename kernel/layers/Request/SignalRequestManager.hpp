#pragma once

#include <arch/arch.h>
#include <stdint.h>
struct ResourceLayerCaps;
struct LogicLayerCaps;

class SignalRequestManager
{
public:
    SignalRequestManager(ResourceLayerCaps* resources, LogicLayerCaps* logic);
    uint64_t HandleRt_sigreturnRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleRt_sigprocmaskRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleRt_sigactionRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSigaltstackRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleKillRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleTgkillRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleQueuedSignalRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePauseRequest(const arch_syscall_frame_t* frame);

private:
    LogicLayerCaps* Logic;
};
