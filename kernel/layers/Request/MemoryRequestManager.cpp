#include "layers/Request/MemoryRequestManager.hpp"

uint64_t MemoryRequestManager::HandleMmapRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t MemoryRequestManager::HandleMunmapRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t MemoryRequestManager::HandleMprotectRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t MemoryRequestManager::HandleMincoreRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t MemoryRequestManager::HandleMadviseRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t MemoryRequestManager::HandleBrkRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}
