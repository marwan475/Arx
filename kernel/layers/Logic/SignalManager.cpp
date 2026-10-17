#include "layers/Logic/SignalManager.hpp"

#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/TimeManager.hpp"

#include <klib/klib.h>

namespace
{
constexpr uint64_t RESTORER = 0x04000000, ONSTACK = 0x08000000, RESTART = 0x10000000, NODEFER = 0x40000000, RESETHAND = 0x80000000;
bool               on_stack(const signal_stack_t& stack, uint64_t rsp)
{
    return !(stack.flags & 2) && rsp >= stack.sp && rsp - stack.sp < stack.size;
}
} // namespace

SignalManager::SignalManager(ResourceLayerCaps* resources, Scheduler* scheduler) : Resources(resources), SchedulerManager(scheduler)
{
}

uint64_t SignalManager::Action(int signal, const signal_action_t* action, signal_action_t* previous)
{
    if (signal < 1 || signal > 64)
    {
        return LINUX_EINVAL;
    }

    if (action != nullptr)
    {
        if (signal == 9 || signal == 19)
        {
            return LINUX_EINVAL;
        }

        if ((action->flags & ~(7ULL | RESTORER | ONSTACK | RESTART | NODEFER | RESETHAND)) != 0)
        {
            return LINUX_EOPNOTSUPP;
        }

        if (action->handler > 1)
        {
            const uint64_t status = arch_signal_validate_action(action);

            if (status != 0)
            {
                return status;
            }
        }
    }

    if (Resources == nullptr || Resources->processManager == nullptr || Resources->taskManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* process = Resources->processManager->AcquireCurrentMemoryProcess(false);

    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t         bit = 1ULL << (signal - 1);
    SignalDispositionGuard signalGuard(process);

    if (previous != nullptr)
    {
        *previous = process->Signals()->actions[signal - 1];
    }

    if (action != nullptr)
    {
        process->Signals()->actions[signal - 1] = *action;
        process->Signals()->actions[signal - 1].mask &= ~SIGNAL_UNBLOCKABLE;
        const bool ignored = action->handler == 1 || (action->handler == 0 && (SIGNAL_DEFAULT_IGNORED & bit));

        if (ignored)
        {
            __atomic_fetch_or(&process->Signals()->ignored, bit, __ATOMIC_RELEASE);
            Discard(&process->signalPending, process->signalQueue, signal);

            for (size_t i = 0; i < TaskManager::MAX_TASKS; ++i)
            {
                task_t* task = &Resources->taskManager->GetTasks()[i];

                if (Resources->taskManager->RetainForDispatch(task))
                {
                    if ((task->process != nullptr && task->process->signalDispositions == process->Signals()) || task == Resources->taskManager->GetCurrentTask())
                    {
                        Discard(&task->signalPending, task->signalQueue, signal);

                        if (task->process != nullptr)
                        {
                            Discard(&task->process->signalPending, task->process->signalQueue, signal);
                        }
                    }

                    Resources->taskManager->ReleaseDispatchClaim(task);
                }
            }
        }
        else
        {
            __atomic_fetch_and(&process->Signals()->ignored, ~bit, __ATOMIC_RELEASE);
        }
    }

    signalGuard.Release();
    Resources->processManager->ReleaseMemoryProcess(process);
    return 0;
}

uint64_t SignalManager::Mask(int operation, const uint64_t* mask, uint64_t* previous)
{
    if (mask != nullptr && (operation < 0 || operation > 2))
    {
        return LINUX_EINVAL;
    }

    if (Resources == nullptr || Resources->taskManager == nullptr || Resources->processManager == nullptr || SchedulerManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    task_t*    task    = Resources->taskManager->GetCurrentTask();
    process_t* process = Resources->processManager->GetCurrentProcess();

    if (task == nullptr || process == nullptr)
    {
        return LINUX_ESRCH;
    }

    uint64_t old = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);

    if (previous != nullptr)
    {
        *previous = old;
    }

    if (mask != nullptr)
    {
        uint64_t value = operation == 0 ? old | *mask : operation == 1 ? old & ~*mask : *mask;
        __atomic_store_n(&task->signalMask, value & ~SIGNAL_UNBLOCKABLE, __ATOMIC_RELEASE);
        SchedulerManager->WakeSignalWaiters(process);
    }

    return 0;
}

uint64_t SignalManager::BeginTemporaryMask(uint64_t mask)
{
    if (Resources == nullptr || Resources->taskManager == nullptr || Resources->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    task_t*    task    = Resources->taskManager->GetCurrentTask();
    process_t* process = Resources->processManager->GetCurrentProcess();

    if (task == nullptr || process == nullptr)
    {
        return LINUX_ESRCH;
    }

    SignalDispositionGuard signalGuard(process);
    task->signalSavedMask   = __atomic_load_n(&task->signalMask, __ATOMIC_RELAXED);
    task->signalRestoreMask = true;
    __atomic_store_n(&task->signalMask, mask & ~SIGNAL_UNBLOCKABLE, __ATOMIC_RELEASE);
    signalGuard.Release();
    return 0;
}

void SignalManager::FinishTemporaryMask(bool deferRestore)
{
    if (Resources == nullptr || Resources->taskManager == nullptr || Resources->processManager == nullptr)
    {
        return;
    }

    task_t*    task    = Resources->taskManager->GetCurrentTask();
    process_t* process = Resources->processManager->GetCurrentProcess();

    if (task == nullptr || process == nullptr || deferRestore)
    {
        return;
    }

    SignalDispositionGuard signalGuard(process);

    if (task->signalRestoreMask)
    {
        __atomic_store_n(&task->signalMask, task->signalSavedMask, __ATOMIC_RELEASE);
        task->signalRestoreMask = false;
    }

    signalGuard.Release();
}

uint64_t SignalManager::Stack(const signal_stack_t* stack, signal_stack_t* previous, uint64_t rsp)
{
    if (Resources == nullptr || Resources->taskManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    task_t* task = Resources->taskManager->GetCurrentTask();

    if (task == nullptr)
    {
        return LINUX_ESRCH;
    }

    const bool active = on_stack(task->signalStack, rsp);

    if (previous != nullptr)
    {
        *previous = task->signalStack;

        if (active)
        {
            previous->flags = 1;
        }
    }

    if (stack != nullptr)
    {
        if (active)
        {
            return (uint64_t) -1;
        }

        if (stack->flags != 0 && stack->flags != 2)
        {
            return LINUX_EINVAL;
        }

        if (stack->flags == 0 && stack->size < 2048)
        {
            return LINUX_ENOMEM;
        }

        if (stack->flags == 0 && !range_within_u64(stack->sp, stack->size, CANONICAL_USER_BASE, CANONICAL_USER_END))
        {
            return LINUX_EFAULT;
        }

        task->signalStack         = *stack;
        task->signalStack.padding = 0;

        if (stack->flags == 2)
        {
            task->signalStack = {0, 2, 0, 0};
        }
    }

    return 0;
}

uint64_t SignalManager::Send(int32_t pid, int32_t tid, int signal, const signal_sender_t* queued)
{
    if (signal < 0 || signal > 64)
    {
        return LINUX_EINVAL;
    }

    if (Resources == nullptr || Resources->processManager == nullptr || Resources->taskManager == nullptr || SchedulerManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    ProcessManager* processes = Resources->processManager;
    if (queued != nullptr && pid <= 0)
    {
        return LINUX_EINVAL;
    }
    if (pid <= 0)
    {
        if (tid >= 0 || pid == INT32_MIN)
        {
            return LINUX_EINVAL;
        }

        process_t* source = processes->GetCurrentProcess();

        if (source == nullptr)
        {
            return LINUX_ESRCH;
        }

        const uint64_t group = pid == 0 ? source->groupId : static_cast<uint64_t>(-static_cast<int64_t>(pid));
        bool delivered       = false;
        uint64_t failure     = LINUX_ESRCH;

        for (size_t index = 1; index < processes->GetCapacity(); ++index)
        {
            process_t* member = processes->AcquireProcess(index);

            if (member == nullptr)
            {
                continue;
            }

            const bool matches = !member->exited && (pid == -1 ? member->id != 1 && member != source : member->groupId == group);

            if (matches)
            {
                const uint64_t result = Send(static_cast<int32_t>(member->id), -1, signal);
                delivered            = delivered || result == 0;
                failure              = result != 0 ? result : failure;
            }

            processes->ReleaseProcess(member);
        }

        return delivered ? 0 : failure;
    }

    process_t*      source    = processes->AcquireCurrentMemoryProcess(false);
    process_t*      target    = processes->AcquireProcess((uint64_t) pid);
    uint64_t        result    = [&]() -> uint64_t
    {
        if (source == nullptr || target == nullptr || target->exited)
        {
            return LINUX_ESRCH;
        }

        process_identity_t sender, receiver;
        spinlock_acquire_preempt(&source->credentialLock);
        sender = source->credentials.user;
        spinlock_release_preempt(&source->credentialLock);
        spinlock_acquire_preempt(&target->credentialLock);
        receiver = target->credentials.user;
        spinlock_release_preempt(&target->credentialLock);

        if (!(signal == 18 && source->sessionId == target->sessionId) && sender.effective != 0 && sender.real != receiver.real && sender.real != receiver.saved && sender.effective != receiver.real && sender.effective != receiver.saved)
        {
            return (uint64_t) -1;
        }

        if (queued != nullptr && (queued->code >= 0 || queued->code == -6) && (target != source || (tid >= 0 && static_cast<uint64_t>(tid) != Resources->taskManager->GetCurrentTask()->id)))
        {
            return static_cast<uint64_t>(-1);
        }

        task_t* task = nullptr;

        if (tid >= 0)
        {
            if ((uint32_t) tid >= TaskManager::MAX_TASKS)
            {
                return LINUX_ESRCH;
            }

            task = Resources->taskManager->FindTask(static_cast<uint32_t>(tid));

            if (!Resources->taskManager->RetainForDispatch(task))
            {
                return LINUX_ESRCH;
            }

            if (task->process != target && !(target == source && task == Resources->taskManager->GetCurrentTask()))
            {
                Resources->taskManager->ReleaseDispatchClaim(task);
                return LINUX_ESRCH;
            }
        }

        uint64_t status = 0;

        if (signal == 18 || signal == 9)
        {
            SchedulerManager->ChangeStoppedState(target, signal, false);
        }

        if (signal == 19)
        {
            SchedulerManager->ChangeStoppedState(target, signal, true);

            if (task != nullptr)
            {
                Resources->taskManager->ReleaseDispatchClaim(task);
            }

            return 0;
        }

        if (signal != 0)
        {
            SignalDispositionGuard signalGuard(target);
            const signal_sender_t  info = {(int32_t) source->id, sender.real, tid >= 0 ? -6 : 0};
            status                      = Queue(target, task, signal, queued != nullptr ? *queued : info);
            signalGuard.Release();
            if (status == 0)
            {
                SchedulerManager->WakeSignalWaiters(target);
            }
        }

        if (task != nullptr)
        {
            Resources->taskManager->ReleaseDispatchClaim(task);
        }

        return status;
    }();

    if (source != nullptr)
    {
        processes->ReleaseMemoryProcess(source);
    }

    if (target != nullptr)
    {
        processes->ReleaseProcess(target);
    }

    return result;
}

uint64_t SignalManager::Pause()
{
    if (Resources == nullptr || Resources->taskManager == nullptr || SchedulerManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    task_t* task = Resources->taskManager->GetCurrentTask();

    if (task == nullptr)
    {
        return LINUX_ESRCH;
    }

    uint64_t sequence = __atomic_load_n(&task->signalSequence, __ATOMIC_ACQUIRE);
    uint64_t result   = SchedulerManager->WaitForEvent(&task->signalSequence, sequence);
    return result == 0 ? (uint64_t) -4 : result;
}

[[noreturn]] void SignalManager::Terminate(int signal)
{
    process_t* process = Resources->processManager->GetCurrentProcess();
    process->terminationSignal = signal;
    (void) SchedulerManager->KillProcess(process->id, 128 + signal);
    SchedulerManager->ExitCurrentTask();
}

uint64_t SignalManager::Deliver(arch_syscall_frame_t* frame, uint64_t result, bool restartable)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Resources == nullptr || Resources->processManager == nullptr || Resources->taskManager == nullptr || SchedulerManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* process = Resources->processManager->GetCurrentProcess();
    task_t*    task    = Resources->taskManager->GetCurrentTask();

    if (process == nullptr || task == nullptr || !arch_signal_user_return(frame))
    {
        return result;
    }

    if (process->exited || task->dying)
    {
        SchedulerManager->ExitCurrentTask();
    }

    while (__atomic_load_n(&process->stopped, __ATOMIC_ACQUIRE) && !process->exited && !task->dying)
    {
        const uint64_t sequence = __atomic_load_n(&process->stopSequence, __ATOMIC_ACQUIRE);

        if (process->stopped)
        {
            (void) SchedulerManager->WaitForEvent(&process->stopSequence, sequence, UINT64_MAX, true);
        }
    }

    if (process->exited || task->dying)
    {
        SchedulerManager->ExitCurrentTask();
    }

    SignalDispositionGuard signalGuard(process);
    const uint64_t         pending = pending_signals(process, task);

    if (pending == 0)
    {
        signalGuard.Release();
        FinishTemporaryMask(false);
        return result;
    }
    signal_sender_t sender = {};
    const int       signal = Peek(process, task, pending, &sender);
    const uint64_t  bit    = 1ULL << (signal - 1);
    signal_action_t action = process->Signals()->actions[signal - 1];
    Consume(process, task, signal);

    if (action.flags & RESETHAND)
    {
        process->Signals()->actions[signal - 1] = {};

        if (SIGNAL_DEFAULT_IGNORED & bit)
        {
            __atomic_fetch_or(&process->Signals()->ignored, bit, __ATOMIC_RELEASE);
        }
    }

    signalGuard.Release();

    if (signal == 14 && Resources->timeManager != nullptr)
    {
        Resources->timeManager->RestartRealTimer(process);
    }

    if (action.handler == 1 || (action.handler == 0 && (SIGNAL_DEFAULT_IGNORED & bit)))
    {
        FinishTemporaryMask(false);
        return result;
    }

    if (action.handler == 0)
    {
        if (signal >= 19 && signal <= 22)
        {
            SchedulerManager->ChangeStoppedState(process, signal, true);
            return Deliver(frame, result, restartable);
        }

        Terminate(signal);
    }

    if (task->signalDepth == UINT64_MAX)
    {
        Terminate(11);
    }

    const uint64_t mask        = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);
    const uint64_t restoreMask = task->signalRestoreMask ? task->signalSavedMask : mask;

    if (arch_signal_prepare(frame, result, &action, &task->signalStack, &sender, restoreMask, signal, restartable) != 0)
    {
        Terminate(11);
    }

    ++task->signalDepth;
    task->signalRestoreMask = false;
    __atomic_store_n(&task->signalMask, (mask | action.mask | ((action.flags & NODEFER) ? 0 : bit)) & ~SIGNAL_UNBLOCKABLE, __ATOMIC_RELEASE);
    return 0;
}

uint64_t SignalManager::Restore(arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Resources == nullptr || Resources->taskManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    task_t* task = Resources->taskManager->GetCurrentTask();

    if (task == nullptr)
    {
        return LINUX_ESRCH;
    }

    uint64_t       result = 0, mask = 0;
    signal_stack_t stack  = {};
    const uint64_t status = task->signalDepth == 0 ? LINUX_EINVAL : arch_signal_restore(frame, &result, &mask, &stack);

    if (status != 0)
    {
        if (task->isUserTask)
        {
            Terminate(11);
        }

        return status;
    }

    __atomic_store_n(&task->signalMask, mask & ~SIGNAL_UNBLOCKABLE, __ATOMIC_RELEASE);
    task->signalStack = stack;
    --task->signalDepth;
    return result;
}

uint64_t SignalManager::Queue(process_t* process, task_t* task, int signal, const signal_sender_t& sender)
{
    if (signal < 1 || signal > 64)
    {
        return LINUX_EINVAL;
    }
    const uint64_t bit = 1ULL << (signal - 1);
    if (process->Signals()->ignored & bit)
    {
        return 0;
    }
    uint64_t* pending = task != nullptr ? &task->signalPending : &process->signalPending;
    if (signal >= 32)
    {
        signal_queue_t*& queue = task != nullptr ? task->signalQueue : process->signalQueue;
        if (queue == nullptr)
        {
            queue = static_cast<signal_queue_t*>(kzalloc(sizeof(signal_queue_t)));
            if (queue == nullptr)
            {
                return LINUX_ENOMEM;
            }
        }
        if (queue->count + queue->reserved == signal_queue_t::CAPACITY)
        {
            return static_cast<uint64_t>(-11);
        }
        queue->records[queue->count++] = {static_cast<uint32_t>(signal), sender};
    }
    else if (!(*pending & bit))
    {
        if (task != nullptr && task->signalSenders == nullptr)
        {
            task->signalSenders = static_cast<signal_sender_t*>(kzalloc(32 * sizeof(signal_sender_t)));
            if (task->signalSenders == nullptr)
            {
                return LINUX_ENOMEM;
            }
        }
        (task != nullptr ? task->signalSenders : process->signalSenders)[signal - 1] = sender;
    }
    __atomic_fetch_or(pending, bit, __ATOMIC_RELEASE);
    return 0;
}

int SignalManager::Peek(process_t* process, task_t* task, uint64_t mask, signal_sender_t* sender)
{
    uint64_t   available = task->signalPending & mask;
    const bool thread    = available != 0;
    if (!thread)
    {
        available = process->signalPending & mask;
    }
    const uint64_t standard = (task->signalPending | process->signalPending) & mask & 0x7fffffffULL;
    if (standard != 0)
    {
        available = standard;
    }
    // SIGKILL cannot be delayed by a lower-numbered thread-directed signal.
    if ((task->signalPending | process->signalPending) & mask & (1ULL << 8))
    {
        available = 1ULL << 8;
    }
    if (available == 0)
    {
        return 0;
    }
    const int  signal     = __builtin_ctzll(available) + 1;
    const bool fromThread = (task->signalPending & (1ULL << (signal - 1))) != 0;
    if (signal >= 32)
    {
        signal_queue_t* queue = fromThread ? task->signalQueue : process->signalQueue;
        for (size_t i = 0; queue != nullptr && i < queue->count; ++i)
        {
            if (queue->records[i].number == static_cast<uint32_t>(signal))
            {
                *sender = queue->records[i].sender;
                break;
            }
        }
    }
    else
    {
        *sender = fromThread && task->signalSenders != nullptr ? task->signalSenders[signal - 1] : process->signalSenders[signal - 1];
    }
    return signal;
}

void SignalManager::Consume(process_t* process, task_t* task, int signal)
{
    const uint64_t  bit       = 1ULL << (signal - 1);
    const bool      thread    = (task->signalPending & bit) != 0;
    uint64_t*       pending   = thread ? &task->signalPending : &process->signalPending;
    signal_queue_t* queue     = thread ? task->signalQueue : process->signalQueue;
    bool            remaining = false;
    bool            removed   = false;
    if (signal >= 32 && queue != nullptr)
    {
        size_t count = 0;
        for (size_t i = 0; i < queue->count; ++i)
        {
            const auto record = queue->records[i];
            if (!removed && record.number == static_cast<uint32_t>(signal))
            {
                removed = true;
                continue;
            }
            queue->records[count++] = record;
            remaining |= record.number == static_cast<uint32_t>(signal);
        }
        queue->count = count;
    }
    if (!remaining)
    {
        __atomic_fetch_and(pending, ~bit, __ATOMIC_RELEASE);
    }
}

void SignalManager::Discard(uint64_t* pending, signal_queue_t* queue, int signal)
{
    if (queue != nullptr && signal >= 32)
    {
        size_t count = 0;
        for (size_t i = 0; i < queue->count; ++i)
        {
            if (queue->records[i].number != static_cast<uint32_t>(signal))
            {
                queue->records[count++] = queue->records[i];
            }
        }
        queue->count = count;
    }
    __atomic_fetch_and(pending, ~(1ULL << (signal - 1)), __ATOMIC_RELEASE);
}

uint64_t SignalManager::ReserveExit(process_t* child, process_t* recipient)
{
    process_t* retained = recipient != nullptr ? Resources->processManager->AcquireProcess(recipient->id) : nullptr;
    if (retained == nullptr)
    {
        return LINUX_ESRCH;
    }
    uint64_t result = 0;
    {
        SignalDispositionGuard guard(retained);
        if (retained->exited)
        {
            result = LINUX_ESRCH;
        }
        else
        {
            if (retained->signalQueue == nullptr)
            {
                retained->signalQueue = static_cast<signal_queue_t*>(kzalloc(sizeof(signal_queue_t)));
            }
            signal_queue_t* queue = retained->signalQueue;
            if (queue == nullptr)
            {
                result = LINUX_ENOMEM;
            }
            else if (queue->count + queue->reserved == signal_queue_t::CAPACITY)
            {
                result = LINUX_EAGAIN;
            }
            else
            {
                ++queue->reserved;
                child->exitRecipient = retained;
            }
        }
    }
    if (result != 0)
    {
        Resources->processManager->ReleaseProcess(retained);
    }
    return result;
}

void SignalManager::ReleaseExit(ResourceLayerCaps* resources, process_t* child, bool deliver)
{
    process_t* recipient = __atomic_exchange_n(&child->exitRecipient, nullptr, __ATOMIC_ACQ_REL);
    if (recipient == nullptr)
    {
        return;
    }
    SignalDispositionGuard guard(recipient);
    signal_queue_t* queue = recipient->signalQueue;
    if (queue != nullptr && queue->reserved != 0)
    {
        --queue->reserved;
        if (deliver && !recipient->exited)
        {
            const signal_sender_t sender = {static_cast<int32_t>(child->id), child->credentials.user.real, child->terminationSignal != 0 ? 2 : 1, static_cast<uint64_t>(child->terminationSignal != 0 ? child->terminationSignal : child->exitStatus)};
            // Reservation guarantees capacity and allocated storage. No exit
            // path blocks or allocates to report this notification.
            (void) Queue(recipient, nullptr, child->exitSignal, sender);
        }
    }
    guard.Release();
    resources->processManager->ReleaseProcess(recipient);
}
