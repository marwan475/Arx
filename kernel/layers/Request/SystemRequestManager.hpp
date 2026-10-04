#pragma once

#include <arch/arch.h>
#include <stdint.h>

class SystemRequestManager
{
public:
    uint64_t Handlearch_prctlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleumaskRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleprlimit64Request(const arch_syscall_frame_t* frame);
    uint64_t HandlegetrlimitRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetrlimitRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleprctlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleunameRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesysinfoRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetrandomRequest(const arch_syscall_frame_t* frame);
};
