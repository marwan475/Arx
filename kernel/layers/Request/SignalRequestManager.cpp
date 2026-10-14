#include "layers/Request/SignalRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SignalManager.hpp"

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
