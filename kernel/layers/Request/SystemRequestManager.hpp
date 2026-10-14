#pragma once

#include <arch/arch.h>
#include <stdint.h>

struct ResourceLayerCaps;

class SystemRequestManager
{
public:
    explicit SystemRequestManager(ResourceLayerCaps* resourceLayerCaps);

    uint64_t HandleArch_prctlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleUmaskRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePrlimit64Request(const arch_syscall_frame_t* frame);
    uint64_t HandleGetrlimitRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetrlimitRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePrctlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleUnameRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSysinfoRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetrandomRequest(const arch_syscall_frame_t* frame);

private:
    uint64_t HandleFileLimitRequest(int32_t pid, uint32_t resource, uint64_t proposed, uint64_t previous);
    ResourceLayerCaps* ResourceCaps;
};
