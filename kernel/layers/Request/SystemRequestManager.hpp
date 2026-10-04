#pragma once

#include <arch/arch.h>
#include <stdint.h>

class SystemRequestManager
{
public:
    uint64_t HandleArch_prctlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleUmaskRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePrlimit64Request(const arch_syscall_frame_t* frame);
    uint64_t HandleGetrlimitRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetrlimitRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePrctlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleUnameRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSysinfoRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetrandomRequest(const arch_syscall_frame_t* frame);
};
