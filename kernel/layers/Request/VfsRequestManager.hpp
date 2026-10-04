#pragma once

#include <arch/arch.h>
#include <stdint.h>

class VfsRequestManager
{
public:
    uint64_t HandleOpenatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleOpenRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleCreatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleCloseRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleClose_rangeRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMkdiratRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMkdirRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleReadRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleWriteRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePread64Request(const arch_syscall_frame_t* frame);
    uint64_t HandlePwrite64Request(const arch_syscall_frame_t* frame);
    uint64_t HandleReadvRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleWritevRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePreadvRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePwritevRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleLseekRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetcwdRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleChdirRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFchdirRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetdents64Request(const arch_syscall_frame_t* frame);
    uint64_t HandleUnlinkatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleUnlinkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleRmdirRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFcntlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleDupRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleDup2Request(const arch_syscall_frame_t* frame);
    uint64_t HandleDup3Request(const arch_syscall_frame_t* frame);
    uint64_t HandleNewfstatatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleStatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFstatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleLstatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleRenameatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleRenameRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleReadlinkatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleReadlinkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleIoctlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleLinkatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleLinkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSymlinkatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSymlinkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFaccessatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFaccessat2Request(const arch_syscall_frame_t* frame);
    uint64_t HandleAccessRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFchownatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleChownRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFchownRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleLchownRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFchmodat2Request(const arch_syscall_frame_t* frame);
    uint64_t HandleFchmodatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleChmodRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFchmodRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMountRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleStatfsRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFstatfsRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMknodatRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleMknodRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleTruncateRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleFtruncateRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePipeRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePipe2Request(const arch_syscall_frame_t* frame);
    uint64_t HandleMemfd_createRequest(const arch_syscall_frame_t* frame);
};
