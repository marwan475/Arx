#pragma once

#include <arch/arch.h>
#include <stdint.h>

class SchedulerRequestManager
{
public:
    uint64_t Handlesched_yieldRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlesched_getaffinityRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetcpuRequest(const arch_syscall_frame_t* frame);
};
