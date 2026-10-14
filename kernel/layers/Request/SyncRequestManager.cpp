#include "layers/Request/SyncRequestManager.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SynchronizationManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TimeManager.hpp"
#include <klib/klib.h>

SyncRequestManager::SyncRequestManager(ResourceLayerCaps* resources, LogicLayerCaps* logic)
    : Resources(resources), Logic(logic)
{
}

uint64_t SyncRequestManager::HandleFutexRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->synchronizationManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    const uint32_t operation = (uint32_t) frame->arg1;
    const uint32_t command = operation & ~384U; // FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME

    if (command != 0 && command != 1 && command != 9 && command != 10)
    {
        return LINUX_ENOSYS;
    }

    const bool wait = command == 0 || command == 9;
    const bool realtime = (operation & 256) != 0;

    if (realtime && command != 9)
    {
        return LINUX_ENOSYS;
    }

    if ((operation & 128) == 0)
    {
        return LINUX_EOPNOTSUPP; // Shared backing/key ownership is absent.
    }

    if ((frame->arg0 & 3) != 0)
    {
        return LINUX_EINVAL;
    }

    const uint32_t bits = command == 9 || command == 10 ? (uint32_t) frame->arg5 : UINT32_MAX;

    if (bits == 0)
    {
        return LINUX_EINVAL;
    }

    if (!wait) return Logic->synchronizationManager->Wake(frame->arg0, (int32_t)
    {
        frame->arg2, bits);
    }

    uint64_t deadline = UINT64_MAX;

    if (frame->arg3 != 0)
    {
        time_timespec_t timeout;

        if (!safe_read_user(frame->arg3, &timeout, sizeof(timeout)))
        {
            return LINUX_EFAULT;
        }

        if (timeout.seconds < 0 || timeout.nanoseconds < 0 || timeout.nanoseconds >= 1000000000)
        {
            return LINUX_EINVAL;
        }

        if (Resources == nullptr || Resources->timeManager == nullptr)
        {
            return LINUX_ENOSYS;
        }

        uint64_t status = Resources->timeManager->CalculateSleepDeadline(realtime ? 0 : 1, command == 9, timeout, &deadline);

        if (status != 0)
        {
            return status;
        }

        if (deadline == UINT64_MAX)
        {
            return LINUX_EOVERFLOW;
        }
    }

    return Logic->synchronizationManager->Wait(frame->arg0, (uint32_t) frame->arg2, bits, deadline);
}

uint64_t SyncRequestManager::HandleSet_robust_listRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->synchronizationManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if (frame->arg1 != 24)
    {
        return LINUX_EINVAL;
    }

    return Logic->synchronizationManager->SetRobustList(frame->arg0);
}

uint64_t SyncRequestManager::HandleGet_robust_listRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (Logic == nullptr || Logic->synchronizationManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    uint64_t head, length;
    uint64_t result = Logic->synchronizationManager->GetRobustList((int32_t) frame->arg0, &head, &length);

    if (result != 0)
    {
        return result;
    }

    if (!safe_write_user(frame->arg2, &length, sizeof(length)) || !safe_write_user(frame->arg1, &head, sizeof(head)))
    {
        return LINUX_EFAULT;
    }

    return 0;
}
