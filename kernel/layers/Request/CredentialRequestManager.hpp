#pragma once

#include <arch/arch.h>
#include <stdint.h>

class CredentialRequestManager
{
public:
    uint64_t HandlegetuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegeteuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetegidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetresuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetresgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetreuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetregidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetresuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetresgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetfsuidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetfsgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetgroupsRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetgroupsRequest(const arch_syscall_frame_t* frame);
};
