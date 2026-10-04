#pragma once

#include <arch/arch.h>
#include <stdint.h>

class SyncRequestManager
{
public:
    uint64_t HandlefutexRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleset_robust_listRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleget_robust_listRequest(const arch_syscall_frame_t* frame);
};
