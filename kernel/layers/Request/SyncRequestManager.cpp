#include "layers/Request/SyncRequestManager.hpp"

uint64_t SyncRequestManager::HandleFutexRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SyncRequestManager::HandleSet_robust_listRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SyncRequestManager::HandleGet_robust_listRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}
