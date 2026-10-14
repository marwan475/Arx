#pragma once
#include "layers/Resource/SignalState.hpp"

#include <arch/arch.h>
#include <stddef.h>

struct ResourceLayerCaps;
class Scheduler;
struct process_t;
struct task_t;

class SignalManager
{
public:
    SignalManager(ResourceLayerCaps* resources, Scheduler* scheduler);
    uint64_t Action(int signal, const signal_action_t* action, signal_action_t* previous);
    uint64_t Mask(int operation, const uint64_t* mask, uint64_t* previous);
    uint64_t Stack(const signal_stack_t* stack, signal_stack_t* previous, uint64_t rsp);
    uint64_t Send(int32_t pid, int32_t tid, int signal);
    uint64_t Pause();
    uint64_t Deliver(arch_syscall_frame_t* frame, uint64_t result, bool restartable = false);
    uint64_t Restore(arch_syscall_frame_t* frame);

private:
    [[noreturn]] void  Terminate(int signal);
    ResourceLayerCaps* Resources;
    Scheduler*         SchedulerManager;
};
