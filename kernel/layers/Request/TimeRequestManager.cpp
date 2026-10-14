#include "layers/Request/TimeRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TimeManager.hpp"

#include <klib/klib.h>

TimeManager* TimeRequestManager::GetTimeManager() const
{
    return ResourceCaps != nullptr ? ResourceCaps->timeManager : nullptr;
}

uint64_t TimeRequestManager::HandleTimeRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr)
        return LINUX_ENOSYS;
    time_timespec_t value;
    uint64_t        result = manager->ReadClock(0, &value);
    if (result != 0)
        return result;
    if (frame->arg0 != 0 && !safe_write_user(frame->arg0, &value.seconds, sizeof(value.seconds)))
        return LINUX_EFAULT;
    return value.seconds;
}

uint64_t TimeRequestManager::HandleGettimeofdayRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr)
        return LINUX_ENOSYS;
    if (frame->arg0 != 0)
    {
        time_timespec_t clock;
        uint64_t        result = manager->ReadClock(0, &clock);
        if (result != 0)
            return result;
        time_timeval_t value = {clock.seconds, clock.nanoseconds / 1000};
        if (!safe_write_user(frame->arg0, &value, sizeof(value)))
            return LINUX_EFAULT;
    }
    if (frame->arg1 != 0)
    {
        const int32_t timezone[2] = {0, 0};
        if (!safe_write_user(frame->arg1, timezone, sizeof(timezone)))
            return LINUX_EFAULT;
    }
    return 0;
}

uint64_t TimeRequestManager::HandleClock_gettimeRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr)
        return LINUX_ENOSYS;
    time_timespec_t value;
    uint64_t        result = manager->ReadClock(static_cast<int32_t>(frame->arg0), &value);
    if (result != 0)
        return result;
    return safe_write_user(frame->arg1, &value, sizeof(value)) ? 0 : LINUX_EFAULT;
}

uint64_t TimeRequestManager::HandleClock_getresRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr)
        return LINUX_ENOSYS;
    time_timespec_t value;
    uint64_t        result = manager->ReadResolution(static_cast<int32_t>(frame->arg0), &value);
    if (result != 0 || frame->arg1 == 0)
        return result;
    return safe_write_user(frame->arg1, &value, sizeof(value)) ? 0 : LINUX_EFAULT;
}

uint64_t TimeRequestManager::HandleClock_nanosleepRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    int32_t clock = static_cast<int32_t>(frame->arg0);
    int32_t flags = static_cast<int32_t>(frame->arg1);
    if ((clock != 0 && clock != 1) || (flags != 0 && flags != 1))
        return LINUX_EINVAL;
    // rem is only written on EINTR; signal delivery is not implemented yet.
    return SleepRequest(clock, flags == 1, frame->arg2);
}

uint64_t TimeRequestManager::HandleNanosleepRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    return SleepRequest(1, false, frame->arg0);
}

uint64_t TimeRequestManager::SleepRequest(int clock, bool absolute, uint64_t requestAddress)
{
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr)
        return LINUX_ENOSYS;
    time_timespec_t request;
    if (!safe_read_user(requestAddress, &request, sizeof(request)))
        return LINUX_EFAULT;
    uint64_t deadline;
    uint64_t result = manager->CalculateSleepDeadline(clock, absolute, request, &deadline);
    if (result != 0)
        return result;
    if (LogicCaps == nullptr || LogicCaps->scheduler == nullptr)
        return LINUX_ENOSYS;
    return LogicCaps->scheduler->SleepUntil(deadline);
}

uint64_t TimeRequestManager::HandleGetitimerRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || static_cast<uint32_t>(frame->arg0) > 2)
        return LINUX_EINVAL;
    TimeManager* manager = GetTimeManager();
    return manager != nullptr ? manager->GetTimer(static_cast<int32_t>(frame->arg0), frame->arg1) : LINUX_ENOSYS;
}

uint64_t TimeRequestManager::HandleSetitimerRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || static_cast<uint32_t>(frame->arg0) > 2)
        return LINUX_EINVAL;
    TimeManager* manager = GetTimeManager();
    return manager != nullptr ? manager->SetTimer(static_cast<int32_t>(frame->arg0), frame->arg1, frame->arg2) : LINUX_ENOSYS;
}

uint64_t TimeRequestManager::HandleAlarmRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    if (static_cast<uint32_t>(frame->arg0) != 0)
        return LINUX_ENOSYS;
    TimeManager* manager = GetTimeManager();
    return manager != nullptr ? manager->CancelAlarm() : LINUX_ENOSYS;
}
