#pragma once

#include <arch/arch.h>
#include <stdint.h>

class VfsRequestManager
{
public:
    uint64_t HandleopenatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleopenRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlecreatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlecloseRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleclose_rangeRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlemkdiratRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlemkdirRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlereadRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlewriteRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlepread64Request(const arch_syscall_frame_t* frame);
    uint64_t Handlepwrite64Request(const arch_syscall_frame_t* frame);
    uint64_t HandlereadvRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlewritevRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlepreadvRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlepwritevRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlelseekRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetcwdRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlechdirRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlefchdirRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlegetdents64Request(const arch_syscall_frame_t* frame);
    uint64_t HandleunlinkatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleunlinkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlermdirRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlefcntlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandledupRequest(const arch_syscall_frame_t* frame);
    uint64_t Handledup2Request(const arch_syscall_frame_t* frame);
    uint64_t Handledup3Request(const arch_syscall_frame_t* frame);
    uint64_t HandlenewfstatatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlestatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlefstatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlelstatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlerenameatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlerenameRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlereadlinkatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlereadlinkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleioctlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlelinkatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlelinkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesymlinkatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesymlinkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlefaccessatRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlefaccessat2Request(const arch_syscall_frame_t* frame);
    uint64_t HandleaccessRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlefchownatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlechownRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlefchownRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlelchownRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlefchmodat2Request(const arch_syscall_frame_t* frame);
    uint64_t HandlefchmodatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlechmodRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlefchmodRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlemountRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlestatfsRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlefstatfsRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlemknodatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlemknodRequest(const arch_syscall_frame_t* frame);
    uint64_t HandletruncateRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleftruncateRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlepipeRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlepipe2Request(const arch_syscall_frame_t* frame);
    uint64_t Handlememfd_createRequest(const arch_syscall_frame_t* frame);
};
