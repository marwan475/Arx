#pragma once

#include <arch/arch.h>
#include <stdint.h>

class SocketRequestManager
{
public:
    uint64_t HandleSocketRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSocketpairRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleBindRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleConnectRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleListenRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleAccept4Request(const arch_syscall_frame_t* frame);
    uint64_t HandleAcceptRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleRecvfromRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleRecvmsgRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSendtoRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSendmsgRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleShutdownRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetsockoptRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetsockoptRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetsocknameRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetpeernameRequest(const arch_syscall_frame_t* frame);
};
