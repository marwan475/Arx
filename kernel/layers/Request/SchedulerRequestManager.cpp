#include "layers/Request/SchedulerRequestManager.hpp"

uint64_t SchedulerRequestManager::HandleSched_yieldRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SchedulerRequestManager::HandleSched_getaffinityRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SchedulerRequestManager::HandleGetcpuRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}
