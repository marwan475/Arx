#include "layers/Request/SignalRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SignalManager.hpp"
#include "layers/Request/RequestLayerFactory.hpp"

#include <klib/klib.h>

SignalRequestManager::SignalRequestManager(ResourceLayerCaps*, LogicLayerCaps* logic) : Logic(logic)
{
}

uint64_t SignalRequestManager::HandleRt_sigreturnRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->signalManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Logic->signalManager->Restore(const_cast<arch_syscall_frame_t*>(frame));
}

uint64_t SignalRequestManager::HandleRt_sigprocmaskRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->signalManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if (frame->arg3 != 8)
    {
        return LINUX_EINVAL;
    }

    uint64_t mask, previous;

    if (frame->arg1 != 0 && !safe_read_user(frame->arg1, &mask, 8))
    {
        return LINUX_EFAULT;
    }

    uint64_t result = Logic->signalManager->Mask((int32_t) frame->arg0, frame->arg1 != 0 ? &mask : nullptr, &previous);

    if (result != 0)
    {
        return result;
    }

    return frame->arg2 != 0 && !safe_write_user(frame->arg2, &previous, 8) ? LINUX_EFAULT : 0;
}

uint64_t SignalRequestManager::HandleRt_sigactionRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->signalManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if (frame->arg3 != 8)
    {
        return LINUX_EINVAL;
    }

    signal_action_t action, previous;

    if (frame->arg1 != 0 && !safe_read_user(frame->arg1, &action, sizeof(action)))
    {
        return LINUX_EFAULT;
    }

    uint64_t result = Logic->signalManager->Action((int32_t) frame->arg0, frame->arg1 != 0 ? &action : nullptr, &previous);

    if (result != 0)
    {
        return result;
    }

    return frame->arg2 != 0 && !safe_write_user(frame->arg2, &previous, sizeof(previous)) ? LINUX_EFAULT : 0;
}

uint64_t SignalRequestManager::HandleSigaltstackRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->signalManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    signal_stack_t stack, previous;

    if (frame->arg0 != 0 && !safe_read_user(frame->arg0, &stack, sizeof(stack)))
    {
        return LINUX_EFAULT;
    }

    uint64_t result = Logic->signalManager->Stack(frame->arg0 != 0 ? &stack : nullptr, &previous, arch_signal_stack_pointer(frame));

    if (result != 0)
    {
        return result;
    }

    return frame->arg1 != 0 && !safe_write_user(frame->arg1, &previous, sizeof(previous)) ? LINUX_EFAULT : 0;
}

uint64_t SignalRequestManager::HandleKillRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->signalManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Logic->signalManager->Send((int32_t) frame->arg0, -1, (int32_t) frame->arg1);
}

uint64_t SignalRequestManager::HandleTgkillRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->signalManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if ((int32_t) frame->arg0 <= 0 || (int32_t) frame->arg1 <= 0)
    {
        return LINUX_EINVAL;
    }

    return Logic->signalManager->Send((int32_t) frame->arg0, (int32_t) frame->arg1, (int32_t) frame->arg2);
}

uint64_t SignalRequestManager::HandlePauseRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->signalManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Logic->signalManager->Pause();
}

uint64_t SignalRequestManager::HandleQueuedSignalRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }
    if (Logic == nullptr || Logic->signalManager == nullptr)
    {
        return LINUX_ENOSYS;
    }
    const bool thread = frame->syscall_number == SYSCALL_rt_tgsigqueueinfo;
    const int32_t pid = static_cast<int32_t>(frame->arg0);
    const int32_t tid = thread ? static_cast<int32_t>(frame->arg1) : -1;
    const int32_t signal = static_cast<int32_t>(thread ? frame->arg2 : frame->arg1);
    if (pid <= 0 || (thread && tid <= 0))
    {
        return LINUX_EINVAL;
    }
    uint8_t info[128];
    if (!safe_read_user(thread ? frame->arg3 : frame->arg2, info, sizeof(info)))
    {
        return LINUX_EFAULT;
    }
    signal_sender_t sender = {};
    memcpy(&sender.code, info + 8, 4);
    memcpy(&sender.pid, info + 16, 4);
    memcpy(&sender.uid, info + 20, 4);
    memcpy(&sender.value, info + 24, 8);
    // Only the kill/queue union is implemented. Timer, poll and fault records
    // have different layouts and must not be silently converted.
    if (sender.code != -1 && sender.code != 0 && sender.code != -6)
    {
        return LINUX_EOPNOTSUPP;
    }
    return Logic->signalManager->Send(pid, tid, signal, &sender);
}
