#pragma once

#include <arch/arch.h>
#include <stdint.h>

class ProcessRequestManager
{
public:
    uint64_t HandleexitRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleexit_groupRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleforkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlevforkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlecloneRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleclone3Request(const arch_syscall_frame_t* frame);
    uint64_t HandleexecveRequest(const arch_syscall_frame_t* frame);
    uint64_t Handlewait4Request(const arch_syscall_frame_t* frame);
    uint64_t HandlewaitidRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleset_tid_addressRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegettidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetpidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetppidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetpgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetpgrpRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetpgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetsidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetsidRequest(const arch_syscall_frame_t* frame);
};
