#include "layers/Request/TimeRequestManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TimeManager.hpp"
#include <klib/klib.h>
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"

TimeManager* TimeRequestManager::GetTimeManager() const
{
    return ResourceCaps != nullptr ? ResourceCaps->timeManager : nullptr;
}

uint64_t TimeRequestManager::HandleTimeRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr) return static_cast<uint64_t>(-22);
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr) return static_cast<uint64_t>(-38);
    time_timespec_t value;
    uint64_t result = manager->ReadClock(0, &value);
    if (result != 0) return result;
    if (frame->arg0 != 0 && !safe_write_user(frame->arg0, &value.seconds, sizeof(value.seconds))) return static_cast<uint64_t>(-14);
    return value.seconds;
}

uint64_t TimeRequestManager::HandleGettimeofdayRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr) return static_cast<uint64_t>(-22);
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr) return static_cast<uint64_t>(-38);
    if (frame->arg0 != 0)
    {
        time_timespec_t clock;
        uint64_t result = manager->ReadClock(0, &clock);
        if (result != 0) return result;
        time_timeval_t value = {clock.seconds, clock.nanoseconds / 1000};
        if (!safe_write_user(frame->arg0, &value, sizeof(value))) return static_cast<uint64_t>(-14);
    }
    if (frame->arg1 != 0)
    {
        const int32_t timezone[2] = {0, 0};
        if (!safe_write_user(frame->arg1, timezone, sizeof(timezone))) return static_cast<uint64_t>(-14);
    }
    return 0;
}

uint64_t TimeRequestManager::HandleClock_gettimeRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr) return static_cast<uint64_t>(-22);
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr) return static_cast<uint64_t>(-38);
    time_timespec_t value;
    uint64_t result = manager->ReadClock(static_cast<int32_t>(frame->arg0), &value);
    if (result != 0) return result;
    return safe_write_user(frame->arg1, &value, sizeof(value)) ? 0 : static_cast<uint64_t>(-14);
}

uint64_t TimeRequestManager::HandleClock_getresRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr) return static_cast<uint64_t>(-22);
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr) return static_cast<uint64_t>(-38);
    time_timespec_t value;
    uint64_t result = manager->ReadResolution(static_cast<int32_t>(frame->arg0), &value);
    if (result != 0 || frame->arg1 == 0) return result;
    return safe_write_user(frame->arg1, &value, sizeof(value)) ? 0 : static_cast<uint64_t>(-14);
}

uint64_t TimeRequestManager::HandleClock_nanosleepRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr) return static_cast<uint64_t>(-22);
    int32_t clock = static_cast<int32_t>(frame->arg0);
    int32_t flags = static_cast<int32_t>(frame->arg1);
    if ((clock != 0 && clock != 1) || (flags != 0 && flags != 1)) return static_cast<uint64_t>(-22);
    // rem is only written on EINTR; signal delivery is not implemented yet.
    return SleepRequest(clock, flags == 1, frame->arg2);
}

uint64_t TimeRequestManager::HandleNanosleepRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr) return static_cast<uint64_t>(-22);
    return SleepRequest(1, false, frame->arg0);
}

uint64_t TimeRequestManager::SleepRequest(int clock, bool absolute, uint64_t requestAddress)
{
    TimeManager* manager = GetTimeManager();
    if (manager == nullptr) return static_cast<uint64_t>(-38);
    time_timespec_t request;
    if (!safe_read_user(requestAddress, &request, sizeof(request))) return static_cast<uint64_t>(-14);
    uint64_t deadline;
    uint64_t result = manager->CalculateSleepDeadline(clock, absolute, request, &deadline);
    if (result != 0) return result;
    if (LogicCaps == nullptr || LogicCaps->scheduler == nullptr) return static_cast<uint64_t>(-38);
    return LogicCaps->scheduler->SleepUntil(deadline);
}

uint64_t TimeRequestManager::HandleGetitimerRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || static_cast<uint32_t>(frame->arg0) > 2) return static_cast<uint64_t>(-22);
    TimeManager* manager = GetTimeManager();
    return manager != nullptr ? manager->GetTimer(static_cast<int32_t>(frame->arg0), frame->arg1) : static_cast<uint64_t>(-38);
}

uint64_t TimeRequestManager::HandleSetitimerRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || static_cast<uint32_t>(frame->arg0) > 2) return static_cast<uint64_t>(-22);
    TimeManager* manager = GetTimeManager();
    return manager != nullptr ? manager->SetTimer(static_cast<int32_t>(frame->arg0), frame->arg1, frame->arg2) : static_cast<uint64_t>(-38);
}

uint64_t TimeRequestManager::HandleAlarmRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr) return static_cast<uint64_t>(-22);
    if (static_cast<uint32_t>(frame->arg0) != 0) return static_cast<uint64_t>(-38);
    TimeManager* manager = GetTimeManager();
    return manager != nullptr ? manager->CancelAlarm() : static_cast<uint64_t>(-38);
}
