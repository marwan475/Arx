#pragma once
#include "layers/Resource/SignalState.hpp"

#include <debug/kernel/stats/SignalDebugStats.hpp>

#include <arch/arch.h>
#include <stddef.h>

struct ResourceLayerCaps;
class Scheduler;
struct process_t;
struct task_t;

class SignalManager
{
public:
    ARX_DEBUG_ONLY(
        static SignalDebugStats DebugStats;
        void PublishDebug(class DebugManager& manager, const ProcessDebugStats* processes);
    )

    SignalManager(ResourceLayerCaps* resources, Scheduler* scheduler);
    // These helpers require the target's SignalDispositionGuard. Pending bits
    // summarize queues; consuming one RT record clears its bit only at the end.
    static uint64_t Queue(process_t* process, task_t* task, int signal, const signal_sender_t& sender);
    static int      Peek(process_t* process, task_t* task, uint64_t mask, signal_sender_t* sender);
    static void     Consume(process_t* process, task_t* task, int signal);
    static void     Discard(uint64_t* pending, signal_queue_t* queue, int signal);
    uint64_t ReserveExit(process_t* child, process_t* recipient);
    static void ReleaseExit(ResourceLayerCaps* resources, process_t* child, bool deliver);
    uint64_t Action(int signal, const signal_action_t* action, signal_action_t* previous);
    uint64_t Mask(int operation, const uint64_t* mask, uint64_t* previous);
    uint64_t BeginTemporaryMask(uint64_t mask);
    void     FinishTemporaryMask(bool deferRestore);
    uint64_t Stack(const signal_stack_t* stack, signal_stack_t* previous, uint64_t rsp);
    uint64_t Send(int32_t pid, int32_t tid, int signal, const signal_sender_t* queued = nullptr);
    uint64_t Pause();
    uint64_t Deliver(arch_syscall_frame_t* frame, uint64_t result, bool restartable = false);
    uint64_t Restore(arch_syscall_frame_t* frame);

private:
    [[noreturn]] void  Terminate(int signal);
    ResourceLayerCaps* Resources;
    Scheduler*         SchedulerManager;
};
