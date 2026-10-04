#pragma once

#include <arch/arch.h>
#include <stdint.h>

class EventRequestManager
{
public:
    uint64_t HandlepollRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleppollRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleselectRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlepselect6Request(const arch_syscall_frame_t* frame);
    uint64_t Handleepoll_createRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleepoll_create1Request(const arch_syscall_frame_t* frame);
    uint64_t Handleepoll_ctlRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleepoll_waitRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleepoll_pwaitRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleepoll_pwait2Request(const arch_syscall_frame_t* frame);
    uint64_t Handleinotify_initRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleinotify_init1Request(const arch_syscall_frame_t* frame);
    uint64_t Handleinotify_add_watchRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleinotify_rm_watchRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleeventfdRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleeventfd2Request(const arch_syscall_frame_t* frame);
    uint64_t HandlesignalfdRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlesignalfd4Request(const arch_syscall_frame_t* frame);
};
