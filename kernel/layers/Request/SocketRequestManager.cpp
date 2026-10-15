#include "layers/Request/SocketRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SocketManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

#include <klib/klib.h>

SocketRequestManager::SocketRequestManager(ResourceLayerCaps* resources, LogicLayerCaps* logic) : Manager(resources != nullptr && resources->processManager != nullptr && logic != nullptr ? logic->socketManager : nullptr)
{
}

uint64_t SocketRequestManager::HandleSocketRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Socket((int32_t) frame->arg0, (int32_t) frame->arg1, (int32_t) frame->arg2);
}

uint64_t SocketRequestManager::HandleSocketpairRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Socketpair((int32_t) frame->arg0, (int32_t) frame->arg1, (int32_t) frame->arg2, frame->arg3);
}

uint64_t SocketRequestManager::HandleBindRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Bind((int32_t) frame->arg0, frame->arg1, (uint32_t) frame->arg2);
}

uint64_t SocketRequestManager::HandleConnectRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Connect((int32_t) frame->arg0, frame->arg1, (uint32_t) frame->arg2);
}

uint64_t SocketRequestManager::HandleListenRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Listen((int32_t) frame->arg0, (int32_t) frame->arg1);
}

uint64_t SocketRequestManager::HandleAccept4Request(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Accept((int32_t) frame->arg0, frame->arg1, frame->arg2, (uint32_t) frame->arg3);
}

uint64_t SocketRequestManager::HandleAcceptRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Accept((int32_t) frame->arg0, frame->arg1, frame->arg2, 0);
}

uint64_t SocketRequestManager::HandleRecvfromRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Recvfrom((int32_t) frame->arg0, frame->arg1, frame->arg2, (uint32_t) frame->arg3, frame->arg4, frame->arg5);
}

uint64_t SocketRequestManager::HandleRecvmsgRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Message((int32_t) frame->arg0, frame->arg1, (uint32_t) frame->arg2, false);
}

uint64_t SocketRequestManager::HandleSendtoRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Sendto((int32_t) frame->arg0, frame->arg1, frame->arg2, (uint32_t) frame->arg3, frame->arg4, (uint32_t) frame->arg5);
}

uint64_t SocketRequestManager::HandleSendmsgRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Message((int32_t) frame->arg0, frame->arg1, (uint32_t) frame->arg2, true);
}

uint64_t SocketRequestManager::HandleShutdownRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Shutdown((int32_t) frame->arg0, (int32_t) frame->arg1);
}

uint64_t SocketRequestManager::HandleGetsockoptRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->GetOption((int32_t) frame->arg0, (int32_t) frame->arg1, (int32_t) frame->arg2, frame->arg3, frame->arg4);
}

uint64_t SocketRequestManager::HandleSetsockoptRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->SetOption((int32_t) frame->arg0, (int32_t) frame->arg1, (int32_t) frame->arg2, frame->arg3, (uint32_t) frame->arg4);
}

uint64_t SocketRequestManager::HandleGetsocknameRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Name((int32_t) frame->arg0, frame->arg1, frame->arg2, false);
}

uint64_t SocketRequestManager::HandleGetpeernameRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return Manager->Name((int32_t) frame->arg0, frame->arg1, frame->arg2, true);
}
