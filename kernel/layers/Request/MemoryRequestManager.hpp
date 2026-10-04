#pragma once

#include <arch/arch.h>
#include <stdint.h>

class MemoryRequestManager
{
public:
    uint64_t HandleMmapRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMunmapRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMprotectRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMincoreRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMadviseRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleBrkRequest(const arch_syscall_frame_t* frame);
};
