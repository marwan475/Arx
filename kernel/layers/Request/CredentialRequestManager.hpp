#pragma once

#include <arch/arch.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct LogicLayerCaps;
class CredentialManager;

class CredentialRequestManager
{
public:
    CredentialRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps);
    uint64_t HandleGetuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGeteuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetegidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetresuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetresgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetreuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetregidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetresuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetresgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetfsuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetfsgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetgroupsRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetgroupsRequest(const arch_syscall_frame_t* frame);

private:
    CredentialManager* GetManager() const;
    uint64_t GetIdentity(const arch_syscall_frame_t* frame, bool group, bool effective);
    uint64_t GetResIdentity(const arch_syscall_frame_t* frame, bool group);
    uint64_t ChangeIdentity(const arch_syscall_frame_t* frame, bool group, uint32_t change);
    ResourceLayerCaps* ResourceCaps;
    LogicLayerCaps* LogicCaps;
};
