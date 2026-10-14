#include "layers/Resource/TimeManager.hpp"

#include "layers/Resource/ProcessManager.hpp"

#include <klib/klib.h>

TimeManager::TimeManager() : RealtimeSeconds(0), ResolutionNanoseconds(0), ClockAvailable(false), RealtimeAvailable(false)
{
    ClockAvailable = arch_time_init(&RealtimeSeconds, &RealtimeAvailable, &ResolutionNanoseconds);
    kprintf("Arx kernel: time clock=%s realtime=%s\n", ClockAvailable ? "HPET" : "unavailable", RealtimeAvailable ? "RTC" : "unavailable");
}

uint64_t TimeManager::ReadClock(int clock, time_timespec_t* value)
{
    if (clock != 0 && clock != 1)
        return LINUX_EINVAL;
    uint64_t elapsed;
    if (!ClockAvailable || (clock == 0 && !RealtimeAvailable) || !arch_time_read_monotonic(&elapsed))
        return LINUX_ENOSYS;
    value->seconds     = elapsed / 1000000000 + (clock == 0 ? RealtimeSeconds : 0);
    value->nanoseconds = elapsed % 1000000000;
    return 0;
}

uint64_t TimeManager::ReadResolution(int clock, time_timespec_t* value)
{
    if (clock != 0 && clock != 1)
        return LINUX_EINVAL;
    if (!ClockAvailable || (clock == 0 && !RealtimeAvailable))
        return LINUX_ENOSYS;
    value->seconds     = ResolutionNanoseconds / 1000000000;
    value->nanoseconds = ResolutionNanoseconds % 1000000000;
    return 0;
}

uint64_t TimeManager::CalculateSleepDeadline(int clock, bool absolute, const time_timespec_t& request, uint64_t* monotonicDeadline)
{
    if ((clock != 0 && clock != 1) || request.seconds < 0 || request.nanoseconds < 0 || request.nanoseconds >= 1000000000)
        return LINUX_EINVAL;
    if (!ClockAvailable || (clock == 0 && !RealtimeAvailable))
        return LINUX_ENOSYS;
    uint64_t elapsed;
    if (!arch_time_read_monotonic(&elapsed))
        return LINUX_ENOSYS;
    uint64_t seconds = static_cast<uint64_t>(request.seconds);
    if (absolute && clock == 0)
    {
        if (seconds < RealtimeSeconds)
        {
            *monotonicDeadline = 0;
            return 0;
        }
        seconds -= RealtimeSeconds;
    }
    if (seconds > (UINT64_MAX - request.nanoseconds) / 1000000000)
        return LINUX_EOVERFLOW;
    uint64_t deadline = seconds * 1000000000 + request.nanoseconds;
    if (!absolute)
    {
        if (deadline > UINT64_MAX - elapsed)
            return LINUX_EOVERFLOW;
        deadline += elapsed;
    }
    *monotonicDeadline = deadline;
    return 0;
}

uint64_t TimeManager::GetTimer(int which, uint64_t destination)
{
    if (which < 0 || which > 2)
        return LINUX_EINVAL;
    user_memory_access_t access;
    if (!begin_user_memory(&access))
        return LINUX_ESRCH;
    process_t*       process = access.process;
    time_itimerval_t value   = {{process->timerIntervalSeconds[which], process->timerIntervalMicroseconds[which]}, {0, 0}};
    bool             copied  = safe_write_user_locked(&access, destination, &value, sizeof(value));
    end_user_memory(&access);
    return copied ? 0 : LINUX_EFAULT;
}

uint64_t TimeManager::SetTimer(int which, uint64_t source, uint64_t previous)
{
    if (which < 0 || which > 2)
        return LINUX_EINVAL;
    user_memory_access_t access;
    if (!begin_user_memory(&access))
        return LINUX_ESRCH;
    process_t*       process = access.process;
    time_itimerval_t value   = {};
    uint64_t         result  = 0;
    if (source != 0 && !safe_read_user_locked(&access, source, &value, sizeof(value)))
        result = LINUX_EFAULT;
    else if (value.interval.seconds < 0 || value.value.seconds < 0 || value.interval.microseconds < 0 || value.interval.microseconds >= 1000000 || value.value.microseconds < 0 || value.value.microseconds >= 1000000)
        result = LINUX_EINVAL;
    else if (value.value.seconds != 0 || value.value.microseconds != 0)
        result = LINUX_EOPNOTSUPP;
    else
    {
        time_itimerval_t old = {{process->timerIntervalSeconds[which], process->timerIntervalMicroseconds[which]}, {0, 0}};
        if (previous != 0 && !safe_write_user_locked(&access, previous, &old, sizeof(old)))
            result = LINUX_EFAULT;
        else
        {
            process->timerIntervalSeconds[which]      = value.interval.seconds;
            process->timerIntervalMicroseconds[which] = value.interval.microseconds;
        }
    }
    end_user_memory(&access);
    return result;
}

uint64_t TimeManager::CancelAlarm()
{
    return SetTimer(0, 0, 0);
}
