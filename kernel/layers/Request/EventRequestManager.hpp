#pragma once

#include <arch/arch.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct LogicLayerCaps;

class EventRequestManager
{
public:
    EventRequestManager(ResourceLayerCaps* resources, LogicLayerCaps* logic);
    uint64_t HandlePollRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePpollRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSelectRequest(const arch_syscall_frame_t* frame);
    uint64_t HandlePselect6Request(const arch_syscall_frame_t* frame);
    uint64_t HandleEpoll_createRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleEpoll_create1Request(const arch_syscall_frame_t* frame);
    uint64_t HandleEpoll_ctlRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleEpoll_waitRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleEpoll_pwaitRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleEpoll_pwait2Request(const arch_syscall_frame_t* frame);
    uint64_t HandleInotify_initRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleInotify_init1Request(const arch_syscall_frame_t* frame);
    uint64_t HandleInotify_add_watchRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleInotify_rm_watchRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleEventfdRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleEventfd2Request(const arch_syscall_frame_t* frame);
    uint64_t HandleSignalfdRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSignalfd4Request(const arch_syscall_frame_t* frame);
private:
    uint64_t WaitRequest(const arch_syscall_frame_t* frame, bool select, bool precise);
    ResourceLayerCaps* Resources;
    LogicLayerCaps* Logic;
};
