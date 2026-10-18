#pragma once
#include "layers/DispatcherBridge.h"

#include <arch/arch.h>
#include <klib/klib.h>
#include <stdint.h>
struct LogicLayerCaps;
struct ResourceLayerCaps;
struct virt_addr_space;
class MemoryRequestManager
{
public:
    explicit MemoryRequestManager(LogicLayerCaps* logicLayerCaps = nullptr) : LogicCaps(logicLayerCaps)
    {
    }

    // Kernel-service ingress is stateless so Resource tests can use it at boot.
    static bool    BeginUserMemory(ResourceLayerCaps* resources, user_memory_access_t* access);
    static void    EndUserMemory(user_memory_access_t* access);
    static void    HandleMinorFault(virt_addr_space* space);
    static int64_t HandleBackingWrite(const dispatcher_backing_operations_t* route, void* context, uint64_t offset, const void* bytes, uint64_t size);
    static void    HandleBackingRelease(const dispatcher_backing_operations_t* route, void* context);

    uint64_t HandleMmapRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMunmapRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMprotectRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMincoreRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMadviseRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleBrkRequest(const arch_syscall_frame_t* frame);

private:
    LogicLayerCaps* LogicCaps;
};
