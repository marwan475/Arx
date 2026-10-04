#pragma once

#include <arch/arch.h>
#include <stdint.h>

class SyncRequestManager
{
public:
    uint64_t HandleFutexRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSet_robust_listRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGet_robust_listRequest(const arch_syscall_frame_t* frame);
};
