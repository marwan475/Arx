#pragma once

#include <arch/arch.h>
#include <stdint.h>

class MemoryRequestManager
{
public:
    uint64_t HandlemmapRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlemunmapRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlemprotectRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlemincoreRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlemadviseRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlebrkRequest(const arch_syscall_frame_t* frame);
};
