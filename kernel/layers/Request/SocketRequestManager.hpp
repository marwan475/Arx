#pragma once

#include <arch/arch.h>
#include <stdint.h>

class SocketRequestManager
{
public:
    uint64_t HandlesocketRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesocketpairRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlebindRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleconnectRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlelistenRequest(const arch_syscall_frame_t* frame);
    uint64_t Handleaccept4Request(const arch_syscall_frame_t* frame);
    uint64_t HandleacceptRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlerecvfromRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlerecvmsgRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesendtoRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesendmsgRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleshutdownRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetsockoptRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlesetsockoptRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetsocknameRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlegetpeernameRequest(const arch_syscall_frame_t* frame);
};
