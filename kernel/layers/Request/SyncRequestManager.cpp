#include "layers/Request/SyncRequestManager.hpp"

uint64_t SyncRequestManager::HandlefutexRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SyncRequestManager::Handleset_robust_listRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SyncRequestManager::Handleget_robust_listRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

