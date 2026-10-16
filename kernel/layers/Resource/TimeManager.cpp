#include "layers/Resource/TimeManager.hpp"

#include "layers/Resource/ProcessManager.hpp"

#include <klib/klib.h>

namespace
{
static bool timeval_nanoseconds(const time_timeval_t& value, uint64_t* result)
{
    const uint64_t microseconds = (uint64_t) value.microseconds;
    const uint64_t seconds      = (uint64_t) value.seconds;

    if (seconds > (UINT64_MAX - microseconds * 1000) / 1000000000)
    {
        return false;
    }

    *result = seconds * 1000000000 + microseconds * 1000;
    return true;
}
} // namespace

TimeManager::TimeManager() : RealtimeSeconds(0), ResolutionNanoseconds(0), ClockAvailable(false), RealtimeAvailable(false)
{
    ClockAvailable = arch_time_init(&RealtimeSeconds, &RealtimeAvailable, &ResolutionNanoseconds);
    kprintf("Arx kernel: time clock=%s realtime=%s\n", ClockAvailable ? "HPET" : "unavailable", RealtimeAvailable ? "RTC" : "unavailable");

    if (!ClockAvailable)
    {
        error_logger("Arx kernel: no usable monotonic clock; clock reads and timed waits are unavailable");
    }
    else if (!RealtimeAvailable)
        error_logger("Arx kernel: RTC unavailable; realtime clock reads are unavailable");
}

uint64_t TimeManager::ReadClock(int clock, time_timespec_t* value)
{
    if (clock != 0 && clock != 1)
    {
        return LINUX_EINVAL;
    }

    uint64_t elapsed;

    if (!ClockAvailable || (clock == 0 && !RealtimeAvailable) || !arch_time_read_monotonic(&elapsed))
    {
        return LINUX_ENOSYS;
    }

    value->seconds     = elapsed / 1000000000 + (clock == 0 ? RealtimeSeconds : 0);
    value->nanoseconds = elapsed % 1000000000;
    return 0;
}

uint64_t TimeManager::ReadResolution(int clock, time_timespec_t* value)
{
    if (clock != 0 && clock != 1)
    {
        return LINUX_EINVAL;
    }

    if (!ClockAvailable || (clock == 0 && !RealtimeAvailable))
    {
        return LINUX_ENOSYS;
    }

    value->seconds     = ResolutionNanoseconds / 1000000000;
    value->nanoseconds = ResolutionNanoseconds % 1000000000;
    return 0;
}

uint64_t TimeManager::CalculateSleepDeadline(int clock, bool absolute, const time_timespec_t& request, uint64_t* monotonicDeadline)
{
    if ((clock != 0 && clock != 1) || request.seconds < 0 || request.nanoseconds < 0 || request.nanoseconds >= 1000000000)
    {
        return LINUX_EINVAL;
    }

    if (!ClockAvailable || (clock == 0 && !RealtimeAvailable))
    {
        return LINUX_ENOSYS;
    }

    uint64_t elapsed;

    if (!arch_time_read_monotonic(&elapsed))
    {
        return LINUX_ENOSYS;
    }

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
    {
        return LINUX_EOVERFLOW;
    }

    uint64_t deadline = seconds * 1000000000 + request.nanoseconds;

    if (!absolute)
    {
        if (deadline > UINT64_MAX - elapsed)
        {
            return LINUX_EOVERFLOW;
        }

        deadline += elapsed;
    }

    *monotonicDeadline = deadline;
    return 0;
}

void TimeManager::SnapshotTimer(process_t* process, int which, uint64_t now, time_itimerval_t* value)
{
    *value = {{process->timerIntervalSeconds[which], process->timerIntervalMicroseconds[which]}, {0, 0}};

    if (which == 0 && process->realTimerArmed)
    {
        const uint64_t remaining = process->realTimerDeadline > now ? process->realTimerDeadline - now : 1000;
        value->value.seconds      = (int64_t) (remaining / 1000000000);
        value->value.microseconds = (int64_t) ((remaining % 1000000000) / 1000);
    }
}

uint64_t TimeManager::GetTimer(int which, uint64_t destination)
{
    if (which < 0 || which > 2)
    {
        return LINUX_EINVAL;
    }

    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        return LINUX_ESRCH;
    }

    process_t*       process = access.process;
    time_itimerval_t value   = {};
    uint64_t         now     = 0;
    uint64_t         result  = 0;
    spinlock_acquire_preempt(&process->timerLock);

    if (which == 0 && process->realTimerArmed && (!ClockAvailable || !arch_time_read_monotonic(&now)))
    {
        result = LINUX_ENOSYS;
    }
    else
    {
        SnapshotTimer(process, which, now, &value);
        result = safe_write_user_locked(&access, destination, &value, sizeof(value)) ? 0 : LINUX_EFAULT;
    }

    spinlock_release_preempt(&process->timerLock);
    end_user_memory(&access);
    return result;
}

uint64_t TimeManager::SetTimer(int which, uint64_t source, uint64_t previous)
{
    if (which < 0 || which > 2)
    {
        return LINUX_EINVAL;
    }

    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        return LINUX_ESRCH;
    }

    process_t*       process = access.process;
    time_itimerval_t value   = {};
    uint64_t         result  = 0;

    if (source != 0 && !safe_read_user_locked(&access, source, &value, sizeof(value)))
    {
        result = LINUX_EFAULT;
    }
    else if (value.interval.seconds < 0 || value.value.seconds < 0 || value.interval.microseconds < 0 || value.interval.microseconds >= 1000000 || value.value.microseconds < 0 || value.value.microseconds >= 1000000)
    {
        result = LINUX_EINVAL;
    }
    else if (which != 0 && (value.value.seconds != 0 || value.value.microseconds != 0))
    {
        result = LINUX_EOPNOTSUPP;
    }
    else
    {
        uint64_t duration = 0;
        uint64_t interval = 0;
        uint64_t now      = 0;
        spinlock_acquire_preempt(&process->timerLock);

        if (which == 0 && (!timeval_nanoseconds(value.value, &duration) || !timeval_nanoseconds(value.interval, &interval)))
        {
            result = LINUX_EOVERFLOW;
        }
        else if (which == 0 && (duration != 0 || (previous != 0 && process->realTimerArmed)) && (!ClockAvailable || !arch_time_read_monotonic(&now)))
        {
            result = LINUX_ENOSYS;
        }
        else if (duration > UINT64_MAX - now)
        {
            result = LINUX_EOVERFLOW;
        }
        else
        {
            time_itimerval_t old = {};
            SnapshotTimer(process, which, now, &old);

            if (which == 0)
            {
                process->realTimerDeadline = duration != 0 ? now + duration : 0;
                process->realTimerInterval = duration != 0 ? interval : 0;
                process->realTimerArmed    = duration != 0;
                process->realTimerExpired  = false;

                if (duration == 0)
                {
                    value.interval = {0, 0};
                }
            }

            process->timerIntervalSeconds[which]      = value.interval.seconds;
            process->timerIntervalMicroseconds[which] = value.interval.microseconds;
            // Linux commits the new timer before copying the old value out.
            result = previous == 0 || safe_write_user_locked(&access, previous, &old, sizeof(old)) ? 0 : LINUX_EFAULT;
        }

        spinlock_release_preempt(&process->timerLock);
    }

    end_user_memory(&access);
    return result;
}

uint64_t TimeManager::CancelAlarm()
{
    return SetAlarm(0);
}

uint64_t TimeManager::SetAlarm(uint32_t seconds)
{
    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        return LINUX_ESRCH;
    }

    process_t*     process  = access.process;
    uint64_t       now      = 0;
    uint64_t       result   = 0;
    const uint64_t duration = (uint64_t) seconds * 1000000000;
    spinlock_acquire_preempt(&process->timerLock);

    if ((seconds != 0 || process->realTimerArmed) && (!ClockAvailable || !arch_time_read_monotonic(&now)))
    {
        result = LINUX_ENOSYS;
    }
    else if (duration > UINT64_MAX - now)
    {
        result = LINUX_EOVERFLOW;
    }
    else
    {
        time_itimerval_t old = {};
        SnapshotTimer(process, 0, now, &old);
        uint64_t remaining = (uint64_t) old.value.seconds;

        if ((remaining == 0 && process->realTimerArmed) || old.value.microseconds >= 500000)
        {
            ++remaining;
        }

        process->realTimerDeadline            = seconds != 0 ? now + duration : 0;
        process->realTimerInterval            = 0;
        process->realTimerArmed               = seconds != 0;
        process->realTimerExpired             = false;
        process->timerIntervalSeconds[0]      = 0;
        process->timerIntervalMicroseconds[0] = 0;
        result = (uint32_t) remaining;
    }

    spinlock_release_preempt(&process->timerLock);
    end_user_memory(&access);
    return result;
}

bool TimeManager::ExpireRealTimer(process_t* process, uint64_t now)
{
    if (process == nullptr)
    {
        return false;
    }

    spinlock_acquire_preempt(&process->timerLock);
    bool expired = false;

    if (process->realTimerArmed && now >= process->realTimerDeadline)
    {
        process->realTimerArmed   = false;
        process->realTimerExpired = true;
        SignalDispositionGuard signalGuard(process);
        const uint64_t         bit = 1ULL << 13; // SIGALRM

        if (!__atomic_load_n(&process->exited, __ATOMIC_ACQUIRE) && (__atomic_load_n(&process->Signals()->ignored, __ATOMIC_RELAXED) & bit) == 0)
        {
            if ((__atomic_load_n(&process->signalPending, __ATOMIC_RELAXED) & bit) == 0)
            {
                process->signalSenders[13] = {0, 0, 128}; // SI_KERNEL
            }

            __atomic_fetch_or(&process->signalPending, bit, __ATOMIC_RELEASE);
            expired = true;
        }

        signalGuard.Release();
    }

    spinlock_release_preempt(&process->timerLock);
    return expired;
}

void TimeManager::RestartRealTimer(process_t* process)
{
    if (process == nullptr)
    {
        return;
    }

    spinlock_acquire_preempt(&process->timerLock);
    uint64_t now = 0;

    if (process->realTimerExpired && process->realTimerInterval != 0 && ClockAvailable && arch_time_read_monotonic(&now))
    {
        const uint64_t deadline = process->realTimerDeadline;
        const uint64_t interval = process->realTimerInterval;
        const uint64_t base     = now >= deadline ? now : deadline;
        const uint64_t elapsed  = base - deadline;
        const uint64_t step     = interval - elapsed % interval;

        if (step <= UINT64_MAX - base)
        {
            process->realTimerDeadline = base + step;
            process->realTimerArmed    = true;
        }
    }

    process->realTimerExpired = false;
    spinlock_release_preempt(&process->timerLock);
}
