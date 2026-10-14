#include "layers/Logic/SignalManager.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TaskManager.hpp"
#include <klib/klib.h>

namespace {
constexpr uint64_t RESTORER=0x04000000, ONSTACK=0x08000000, RESTART=0x10000000, NODEFER=0x40000000, RESETHAND=0x80000000;
bool on_stack(const signal_stack_t& stack, uint64_t rsp) { return !(stack.flags & 2) && rsp >= stack.sp && rsp - stack.sp < stack.size; }
}

SignalManager::SignalManager(ResourceLayerCaps* resources, Scheduler* scheduler) : Resources(resources), SchedulerManager(scheduler) {}

uint64_t SignalManager::Action(int signal, const signal_action_t* action, signal_action_t* previous)
{
    if (signal < 1 || signal > 64) return LINUX_EINVAL;
    if (signal >= 32)
    {
        if (action != nullptr) return LINUX_EOPNOTSUPP;
        if (previous != nullptr) *previous={};
        return 0;
    }
    if (action != nullptr)
    {
        if (signal == 9 || signal == 19) return LINUX_EINVAL;
        if ((action->flags & ~(4ULL | RESTORER | ONSTACK | RESTART | NODEFER | RESETHAND)) != 0) return LINUX_EOPNOTSUPP;
        if (action->handler > 1)
        {
            const uint64_t status = arch_signal_validate_action(action);
            if (status != 0) return status;
        }
    }
    if (Resources == nullptr || Resources->processManager == nullptr || Resources->taskManager == nullptr) return LINUX_ENOSYS;
    process_t* process = Resources->processManager->AcquireCurrentMemoryProcess(false);
    if (process == nullptr) return LINUX_ESRCH;
    const uint64_t bit = 1ULL << (signal - 1);
    spinlock_acquire_preempt(&process->signalLock);
    if (previous != nullptr) *previous = process->signalActions[signal-1];
    if (action != nullptr)
    {
        process->signalActions[signal-1] = *action;
        process->signalActions[signal-1].mask &= ~SIGNAL_UNBLOCKABLE;
        const bool ignored = action->handler == 1 || (action->handler == 0 && (SIGNAL_DEFAULT_IGNORED & bit));
        if (ignored)
        {
            __atomic_fetch_or(&process->signalIgnored, bit, __ATOMIC_RELEASE);
            __atomic_fetch_and(&process->signalPending, ~bit, __ATOMIC_RELEASE);
            for (size_t i=0; i<TaskManager::MAX_TASKS; ++i)
            {
                task_t* task=&Resources->taskManager->GetTasks()[i];
                if (Resources->taskManager->RetainForDispatch(task))
                {
                    if (task->process == process || task == Resources->taskManager->GetCurrentTask()) __atomic_fetch_and(&task->signalPending, ~bit, __ATOMIC_RELEASE);
                    Resources->taskManager->ReleaseDispatchClaim(task);
                }
            }
        }
        else __atomic_fetch_and(&process->signalIgnored, ~bit, __ATOMIC_RELEASE);
    }
    spinlock_release_preempt(&process->signalLock);
    Resources->processManager->ReleaseMemoryProcess(process);
    return 0;
}

uint64_t SignalManager::Mask(int operation, const uint64_t* mask, uint64_t* previous)
{
    if (mask != nullptr && (operation < 0 || operation > 2)) return LINUX_EINVAL;
    if (Resources == nullptr || Resources->taskManager == nullptr || Resources->processManager == nullptr || SchedulerManager == nullptr) return LINUX_ENOSYS;
    task_t* task = Resources->taskManager->GetCurrentTask();
    process_t* process = Resources->processManager->GetCurrentProcess();
    if (task == nullptr || process == nullptr) return LINUX_ESRCH;
    uint64_t old = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);
    if (previous != nullptr) *previous = old;
    if (mask != nullptr)
    {
        uint64_t value = operation == 0 ? old | *mask : operation == 1 ? old & ~*mask : *mask;
        __atomic_store_n(&task->signalMask, value & ~SIGNAL_UNBLOCKABLE, __ATOMIC_RELEASE);
        SchedulerManager->WakeSignalWaiters(process);
    }
    return 0;
}

uint64_t SignalManager::Stack(const signal_stack_t* stack, signal_stack_t* previous, uint64_t rsp)
{
    if (Resources == nullptr || Resources->taskManager == nullptr) return LINUX_ENOSYS;
    task_t* task = Resources->taskManager->GetCurrentTask();
    if (task == nullptr) return LINUX_ESRCH;
    const bool active = on_stack(task->signalStack, rsp);
    if (previous != nullptr)
    {
        *previous = task->signalStack;
        if (active) previous->flags = 1;
    }
    if (stack != nullptr)
    {
        if (active) return (uint64_t) -1;
        if (stack->flags != 0 && stack->flags != 2) return LINUX_EINVAL;
        if (stack->flags == 0 && stack->size < 2048) return LINUX_ENOMEM;
        if (stack->flags == 0 && !range_within_u64(stack->sp, stack->size, CANONICAL_USER_BASE, CANONICAL_USER_END)) return LINUX_EFAULT;
        task->signalStack = *stack;
        task->signalStack.padding = 0;
        if (stack->flags == 2) task->signalStack = {0,2,0,0};
    }
    return 0;
}

uint64_t SignalManager::Send(int32_t pid, int32_t tid, int signal)
{
    if (signal < 0 || signal > 64) return LINUX_EINVAL;
    if (pid <= 0) return LINUX_EOPNOTSUPP; // Process groups/sessions are not implemented.
    if (Resources == nullptr || Resources->processManager == nullptr || Resources->taskManager == nullptr || SchedulerManager == nullptr) return LINUX_ENOSYS;
    ProcessManager* processes = Resources->processManager;
    process_t* source = processes->AcquireCurrentMemoryProcess(false);
    process_t* target = processes->AcquireProcess((uint64_t) pid);
    uint64_t result = [&]() -> uint64_t
    {
        if (source == nullptr || target == nullptr || target->exited) return LINUX_ESRCH;
        process_identity_t sender, receiver;
        spinlock_acquire_preempt(&source->credentialLock); sender=source->credentials.user; spinlock_release_preempt(&source->credentialLock);
        spinlock_acquire_preempt(&target->credentialLock); receiver=target->credentials.user; spinlock_release_preempt(&target->credentialLock);
        if (sender.effective != 0 && sender.real != receiver.real && sender.real != receiver.saved && sender.effective != receiver.real && sender.effective != receiver.saved) return (uint64_t) -1;
        task_t* task = nullptr;
        if (tid >= 0)
        {
            if ((uint32_t) tid >= TaskManager::MAX_TASKS) return LINUX_ESRCH;
            task = &Resources->taskManager->GetTasks()[tid];
            if (!Resources->taskManager->RetainForDispatch(task)) return LINUX_ESRCH;
            if (task->process != target && !(target == source && task == Resources->taskManager->GetCurrentTask()))
            {
                Resources->taskManager->ReleaseDispatchClaim(task);
                return LINUX_ESRCH;
            }
        }
        uint64_t status = 0;
        if (signal >= 32 || (signal >= 18 && signal <= 22)) status = LINUX_EOPNOTSUPP; // RT queues and stop/continue are pending.
        else if (signal != 0)
        {
            const uint64_t bit = 1ULL << (signal - 1);
            spinlock_acquire_preempt(&target->signalLock);
            const bool ignored=(__atomic_load_n(&target->signalIgnored,__ATOMIC_RELAXED)&bit)!=0;
            if (!ignored && task != nullptr && task->signalSenders == nullptr)
            {
                task->signalSenders=static_cast<signal_sender_t*>(kzalloc(32*sizeof(signal_sender_t)));
                if (task->signalSenders == nullptr) status=LINUX_ENOMEM;
            }
            if (status == 0 && !ignored)
            {
                signal_sender_t info={(int32_t) source->id, sender.real, tid >= 0 ? -6 : 0};
                if (task != nullptr)
                {
                    if (!(__atomic_load_n(&task->signalPending,__ATOMIC_RELAXED)&bit)) task->signalSenders[signal-1]=info;
                    __atomic_fetch_or(&task->signalPending, bit, __ATOMIC_RELEASE);
                }
                else
                {
                    if (!(__atomic_load_n(&target->signalPending,__ATOMIC_RELAXED)&bit)) target->signalSenders[signal-1]=info;
                    __atomic_fetch_or(&target->signalPending, bit, __ATOMIC_RELEASE);
                }
            }
            spinlock_release_preempt(&target->signalLock);
            SchedulerManager->WakeSignalWaiters(target);
        }
        if (task != nullptr) Resources->taskManager->ReleaseDispatchClaim(task);
        return status;
    }();
    if (source != nullptr) processes->ReleaseMemoryProcess(source);
    if (target != nullptr) processes->ReleaseMemoryProcess(target);
    return result;
}

uint64_t SignalManager::Pause()
{
    if (Resources == nullptr || Resources->taskManager == nullptr || SchedulerManager == nullptr) return LINUX_ENOSYS;
    task_t* task = Resources->taskManager->GetCurrentTask();
    if (task == nullptr) return LINUX_ESRCH;
    uint64_t sequence = __atomic_load_n(&task->signalSequence, __ATOMIC_ACQUIRE);
    uint64_t result = SchedulerManager->WaitForEvent(&task->signalSequence, sequence);
    return result == 0 ? (uint64_t) -4 : result;
}

[[noreturn]] void SignalManager::Terminate(int signal)
{
    process_t* process = Resources->processManager->GetCurrentProcess();
    (void) SchedulerManager->KillProcess(process->id, 128 + signal);
    SchedulerManager->ExitCurrentTask();
}

uint64_t SignalManager::Deliver(arch_syscall_frame_t* frame, uint64_t result, bool restartable)
{
    if (frame == nullptr) return LINUX_EINVAL;
    if (Resources == nullptr || Resources->processManager == nullptr || Resources->taskManager == nullptr || SchedulerManager == nullptr) return LINUX_ENOSYS;
    process_t* process = Resources->processManager->GetCurrentProcess();
    task_t* task = Resources->taskManager->GetCurrentTask();
    if (process == nullptr || task == nullptr || !arch_signal_user_return(frame)) return result;
    if (process->exited) SchedulerManager->ExitCurrentTask();
    spinlock_acquire_preempt(&process->signalLock);
    const uint64_t pending = pending_signals(process, task);
    if (pending == 0) { spinlock_release_preempt(&process->signalLock); return result; }
    const int signal = (pending & (1ULL<<8)) ? 9 : __builtin_ctzll(pending) + 1;
    const uint64_t bit = 1ULL << (signal - 1);
    signal_action_t action = process->signalActions[signal-1];
    signal_sender_t sender;
    if (__atomic_load_n(&task->signalPending, __ATOMIC_RELAXED) & bit)
    { sender=task->signalSenders[signal-1]; __atomic_fetch_and(&task->signalPending, ~bit, __ATOMIC_RELEASE); }
    else { sender=process->signalSenders[signal-1]; __atomic_fetch_and(&process->signalPending, ~bit, __ATOMIC_RELEASE); }
    if (action.flags & RESETHAND)
    {
        process->signalActions[signal-1] = {};
        if (SIGNAL_DEFAULT_IGNORED & bit) __atomic_fetch_or(&process->signalIgnored,bit,__ATOMIC_RELEASE);
    }
    spinlock_release_preempt(&process->signalLock);
    if (action.handler == 1 || (action.handler == 0 && (SIGNAL_DEFAULT_IGNORED & bit))) return result;
    if (action.handler == 0) Terminate(signal);
    if (task->signalDepth == UINT64_MAX) Terminate(11);
    const uint64_t mask = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);
    if (arch_signal_prepare(frame, result, &action, &task->signalStack, &sender, mask, signal, restartable) != 0) Terminate(11);
    ++task->signalDepth;
    __atomic_store_n(&task->signalMask, (mask | action.mask | ((action.flags & NODEFER) ? 0 : bit)) & ~SIGNAL_UNBLOCKABLE, __ATOMIC_RELEASE);
    return 0;
}

uint64_t SignalManager::Restore(arch_syscall_frame_t* frame)
{
    if (frame == nullptr) return LINUX_EINVAL;
    if (Resources == nullptr || Resources->taskManager == nullptr) return LINUX_ENOSYS;
    task_t* task=Resources->taskManager->GetCurrentTask();
    if (task == nullptr) return LINUX_ESRCH;
    uint64_t result = 0, mask = 0;
    signal_stack_t stack = {};
    const uint64_t status = task->signalDepth == 0 ? LINUX_EINVAL : arch_signal_restore(frame, &result, &mask, &stack);
    if (status != 0)
    {
        if (task->isUserTask) Terminate(11);
        return status;
    }
    __atomic_store_n(&task->signalMask, mask & ~SIGNAL_UNBLOCKABLE, __ATOMIC_RELEASE);
    task->signalStack = stack;
    --task->signalDepth;
    return result;
}
