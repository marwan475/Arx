#include "layers/Resource/TimeManager.hpp"

#include "layers/Resource/ProcessManager.hpp"

#include <klib/klib.h>

namespace
{
static bool initialize_platform_clock(void*, uint64_t* seconds, bool* available, uint64_t* resolution)
{
    return arch_time_init(seconds, available, resolution);
}

static bool read_platform_clock(void*, uint64_t* nanoseconds)
{
    return arch_time_read_monotonic(nanoseconds);
}

static uint64_t cpu_clock(const process_t* process, int which)
{
    const uint64_t user   = __atomic_load_n(&process->userCpuNanoseconds, __ATOMIC_RELAXED);
    const uint64_t kernel = which == 1 ? 0 : __atomic_load_n(&process->kernelCpuNanoseconds, __ATOMIC_RELAXED);
    return kernel > UINT64_MAX - user ? UINT64_MAX : user + kernel;
}

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

TimeManager::TimeManager() : TimeManager({initialize_platform_clock, read_platform_clock, nullptr})
{
    kprintf("Arx kernel: time clock=%s realtime=%s\n", ClockAvailable ? "available" : "unavailable", RealtimeAvailable ? "RTC" : "unavailable");

    if (!ClockAvailable)
    {
        error_logger("Arx kernel: no usable monotonic clock; clock reads and timed waits are unavailable");
    }
    else if (!RealtimeAvailable)
        error_logger("Arx kernel: RTC unavailable; realtime clock reads are unavailable");
}

TimeManager::TimeManager(const time_clock_backend_t& backend) : RealtimeSeconds(0), ResolutionNanoseconds(0), ClockAvailable(false), RealtimeAvailable(false), Backend(backend)
{
    ClockAvailable = Backend.initialize != nullptr && Backend.read != nullptr && Backend.initialize(Backend.context, &RealtimeSeconds, &RealtimeAvailable, &ResolutionNanoseconds) && ResolutionNanoseconds != 0;
}

bool TimeManager::ReadMonotonic(uint64_t* nanoseconds) const
{
    return ClockAvailable && Backend.read(Backend.context, nanoseconds);
}

uint64_t TimeManager::ReadClock(int clock, time_timespec_t* value)
{
    // RAW shares the unadjusted hardware epoch. No frequency discipline or
    // suspend accounting is implemented. CPU clocks await a complete live
    // accounting snapshot; elapsed wall time must never stand in for CPU time.
    if (clock != 0 && clock != 1 && clock != 4)
    {
        return LINUX_EINVAL;
    }

    uint64_t elapsed;

    if ((clock == 0 && !RealtimeAvailable) || !ReadMonotonic(&elapsed))
    {
        return LINUX_ENOSYS;
    }

    const uint64_t seconds = elapsed / 1000000000;

    if (clock == 0 && (RealtimeSeconds > INT64_MAX || seconds > (uint64_t) INT64_MAX - RealtimeSeconds))
    {
        return LINUX_EOVERFLOW;
    }

    value->seconds     = seconds + (clock == 0 ? RealtimeSeconds : 0);
    value->nanoseconds = elapsed % 1000000000;
    return 0;
}

uint64_t TimeManager::ReadResolution(int clock, time_timespec_t* value)
{
    if (clock != 0 && clock != 1 && clock != 4)
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

    if (!ClockAvailable || (absolute && clock == 0 && !RealtimeAvailable))
    {
        return LINUX_ENOSYS;
    }

    uint64_t elapsed;

    if (!ReadMonotonic(&elapsed))
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
    else if (which != 0 && process->cpuTimerDeadline[which - 1] != 0)
    {
        const uint64_t deadline   = process->cpuTimerDeadline[which - 1];
        const uint64_t remaining  = deadline > now ? deadline - now : 1000;
        value->value.seconds      = remaining / 1000000000;
        value->value.microseconds = (remaining % 1000000000) / 1000;
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
    TaskManager::AccountActive(false);
    spinlock_acquire_preempt(&process->timerLock);

    if (((which == 0 && process->realTimerArmed) || which != 0) && !ReadMonotonic(&now))
    {
        result = LINUX_ENOSYS;
    }
    else
    {
        SnapshotTimer(process, which, which == 0 ? now : cpu_clock(process, which), &value);
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
    else
    {
        uint64_t duration = 0;
        uint64_t interval = 0;
        uint64_t now      = 0;
        TaskManager::AccountActive(false);
        const uint64_t cpuNow = which == 0 ? 0 : cpu_clock(process, which);
        spinlock_acquire_preempt(&process->timerLock);

        if (!timeval_nanoseconds(value.value, &duration) || !timeval_nanoseconds(value.interval, &interval))
        {
            result = LINUX_EOVERFLOW;
        }
        else if ((which != 0 || duration != 0 || (previous != 0 && process->realTimerArmed)) && !ReadMonotonic(&now))
        {
            result = LINUX_ENOSYS;
        }
        else if (duration > UINT64_MAX - (which == 0 ? now : cpuNow))
        {
            result = LINUX_EOVERFLOW;
        }
        else
        {
            time_itimerval_t old = {};
            SnapshotTimer(process, which, which == 0 ? now : cpuNow, &old);

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
            else
            {
                process->cpuTimerDeadline[which - 1] = duration == 0 ? 0 : cpuNow + duration;
                process->cpuTimerInterval[which - 1] = duration == 0 ? 0 : interval;

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

bool TimeManager::ExpireCpuTimers(process_t* process)
{
    if (process == nullptr || __atomic_load_n(&process->exited, __ATOMIC_ACQUIRE))
    {
        return false;
    }

    spinlock_acquire_preempt(&process->timerLock);
    uint64_t signals = 0;

    for (int index = 0; index < 2; ++index)
    {
        const uint64_t elapsed  = cpu_clock(process, index + 1);
        const uint64_t deadline = process->cpuTimerDeadline[index];

        if (deadline != 0 && elapsed >= deadline)
        {
            signals |= 1ULL << (index == 0 ? 25 : 26); // SIGVTALRM, SIGPROF
            const uint64_t interval = process->cpuTimerInterval[index];
            const uint64_t step     = interval == 0 ? 0 : interval - (elapsed - deadline) % interval;
            process->cpuTimerDeadline[index] = step != 0 && step <= UINT64_MAX - elapsed ? elapsed + step : 0;
        }
    }

    const uint64_t seconds = cpu_clock(process, 2) / 1000000000;

    if (process->cpuLimit.maximum != UINT64_MAX && seconds >= (process->cpuLimit.maximum == 0 ? 1 : process->cpuLimit.maximum))
    {
        signals |= 1ULL << 8; // SIGKILL
    }
    else if (process->cpuLimit.current != UINT64_MAX && seconds >= (process->cpuLimit.current == 0 ? 1 : process->cpuLimit.current) && seconds >= process->cpuLimitNotification)
    {
        signals |= 1ULL << 23; // SIGXCPU, at most once per consumed CPU second.
        process->cpuLimitNotification = seconds + 1;
    }

    bool published = false;

    if (signals != 0)
    {
        SignalDispositionGuard guard(process);
        // SIGKILL cannot be ignored, even if a malformed disposition is present.
        signals &= ~(process->Signals()->ignored & ~(1ULL << 8));

        for (unsigned index = 0; index < 32; ++index)
        {
            if ((signals & (1ULL << index)) != 0 && (process->signalPending & (1ULL << index)) == 0)
            {
                process->signalSenders[index] = {0, 0, 128}; // SI_KERNEL
            }
        }

        __atomic_fetch_or(&process->signalPending, signals, __ATOMIC_RELEASE);
        published = signals != 0;
    }

    spinlock_release_preempt(&process->timerLock);
    return published;
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

    if ((seconds != 0 || process->realTimerArmed) && !ReadMonotonic(&now))
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

    if (process->realTimerExpired && process->realTimerInterval != 0 && ReadMonotonic(&now))
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
