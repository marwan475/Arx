#include "layers/Request/SchedulerRequestManager.hpp"

uint64_t SchedulerRequestManager::Handlesched_yieldRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SchedulerRequestManager::Handlesched_getaffinityRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SchedulerRequestManager::HandlegetcpuRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

