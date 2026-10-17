#pragma once

#include <arch/arch.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct LogicLayerCaps;

class ProcessRequestManager
{
public:
    ProcessRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps);

    uint64_t HandleExitRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleExit_groupRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleForkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleVforkRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleCloneRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleClone3Request(const arch_syscall_frame_t* frame);
    uint64_t HandleExecveRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleWait4Request(const arch_syscall_frame_t* frame);
    uint64_t HandleWaitidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSet_tid_addressRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGettidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetpidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetppidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetpgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetpgrpRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetpgidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleGetsidRequest(const arch_syscall_frame_t* frame);
    uint64_t HandleSetsidRequest(const arch_syscall_frame_t* frame);

private:
    ResourceLayerCaps* ResourceCaps;
    LogicLayerCaps*    LogicCaps;
};
