#include <klib/clock.h>
#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/TimeManager.hpp"

namespace
{
struct counter_fixture_t
{
    uint64_t        value;
    uint64_t        step;
    clock_source_t* interleave;
    uint64_t        nested;
};

static uint64_t read_counter(void* context)
{
    auto*          counter = static_cast<counter_fixture_t*>(context);
    const uint64_t sample  = counter->value;
    counter->value += counter->step;

    if (counter->interleave != nullptr)
    {
        clock_source_t* source = counter->interleave;
        counter->interleave    = nullptr;
        (void) clock_source_read(source, &counter->nested);
    }

    return sample;
}

struct time_backend_fixture_t
{
    bool     available;
    bool     realtime;
    uint64_t now;
    uint64_t seconds;
};

static bool initialize_time(void* context, uint64_t* seconds, bool* available, uint64_t* resolution)
{
    auto* fixture = static_cast<time_backend_fixture_t*>(context);
    *seconds      = fixture->seconds;
    *available    = fixture->realtime;
    *resolution   = 280;
    return fixture->available;
}

static bool read_time(void* context, uint64_t* nanoseconds)
{
    auto* fixture = static_cast<time_backend_fixture_t*>(context);
    *nanoseconds  = fixture->now;
    return fixture->available;
}
} // namespace

void test_TimeManager(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->timeManager : nullptr;
    ManagerSuite suite("resource/TimeManager", m != nullptr);
    suite.Run("resource/TimeManager/source_selection_and_conversion", [&](TestCase& t)
    {
        counter_fixture_t stopped   = {}, running = {0, 1, nullptr, 0};
        clock_source_t candidates[] = {
            {},
            {read_counter, &stopped, UINT32_MAX, 100000000, 1000000, 0, 0, 0},
            {read_counter, &running, 0xffffff, 1000000000, 3579545, 0, 0, 0},
        };
        t.Check(clock_source_select(candidates, 3) == &candidates[2], "absent and stuck HPET fall back to an advancing PM timer");
        t.Check(clock_source_select(candidates, 2) == nullptr, "no usable source is an explicit failure");
        uint64_t value = 123;
        t.Check(clock_ticks_to_nanoseconds(3579545ULL * 60, 1000000000, 3579545, &value) && value == 60000000000ULL, "PM timer retains exact long-interval conversion");
        t.Check(clock_ticks_to_nanoseconds(1, 1000000000, 3579545, &value) && value == 279, "PM timer fractional tick is not rounded up per sample");
        t.Check(clock_ticks_to_nanoseconds(1000001, 33333333, 1000000, &value) && value == 33333366333333ULL / 1000000, "HPET femtosecond fraction retained");
        t.Check(clock_ticks_to_nanoseconds(UINT64_MAX, 1, 1, &value) && value == UINT64_MAX, "maximum representable conversion");
        value = 123;
        t.Check(!clock_ticks_to_nanoseconds(UINT64_MAX, 1000000000, 3579545, &value) && value == 123, "conversion overflow leaves output untouched");
        t.Check(!clock_ticks_to_nanoseconds(1, 1, 0, &value), "zero frequency rejected");
    });

    suite.Run("resource/TimeManager/source_wrap_and_interleaving", [&](TestCase& t)
    {
        const uint64_t masks[] = {0xffffff, UINT32_MAX, UINT64_MAX};

        for (uint64_t mask : masks)
        {
            counter_fixture_t counter = {mask - 2, 1, nullptr, 0};
            clock_source_t    source  = {read_counter, &counter, mask, 1, 1, 0, 0, 0};
            t.Check(clock_source_start(&source), "start near hardware wrap");
            uint64_t sample = 0;
            t.Check(clock_source_read(&source, &sample) && sample == 1, "sample immediately before wrap");
            t.Check(clock_source_read(&source, &sample) && sample == 2, "wrap preserves elapsed epoch");
            t.Check(clock_source_read(&source, &sample) && sample == 3, "sample after wrap");
            counter.interleave = &source;
            t.Check(clock_source_read(&source, &sample) && sample >= counter.nested && sample == 6, "racing reader retries with fresh sample without inventing a wrap");
            counter.step = 0;
            t.Check(clock_source_read(&source, &sample), "unchanged sample is valid");
            const uint64_t previous = sample;
            t.Check(clock_source_read(&source, &sample) && sample == previous, "unchanged counter does not advance epoch");
        }

        counter_fixture_t counter = {0, 0, nullptr, 0};
        clock_source_t    source  = {read_counter, &counter, UINT64_MAX, 1, 1, 0, UINT64_MAX - 1, 0};
        uint64_t          sample  = 123;
        t.Check(!clock_source_read(&source, &sample) && sample == 123, "extended tick overflow cannot wrap monotonic time");
    });

    suite.Run("resource/TimeManager/missing_rtc_and_deadlines", [&](TestCase& t)
    {
        time_backend_fixture_t fixture = {true, false, 1000000001, 0};
        TimeManager            clock({initialize_time, read_time, &fixture});
        time_timespec_t        value    = {}, request = {2, 3};
        uint64_t               deadline = 0;
        t.Equal(clock.ReadClock(0, &value), LINUX_ENOSYS, "missing RTC does not invent a UTC epoch");
        t.Equal(clock.ReadResolution(0, &value), LINUX_ENOSYS, "missing UTC resolution rejected");
        t.Equal(clock.ReadClock(1, &value), 0, "missing RTC preserves monotonic clock");
        t.Check(value.seconds == 1 && value.nanoseconds == 1, "monotonic value normalized");
        t.Equal(clock.ReadClock(4, &value), 0, "raw hardware clock supported");
        t.Equal(clock.ReadResolution(4, &value), 0, "raw resolution supported");
        t.Check(value.seconds == 0 && value.nanoseconds == 280, "fallback resolution is rounded up");
        t.Equal(clock.CalculateSleepDeadline(0, false, request, &deadline), 0, "relative realtime sleep does not require RTC");
        t.Equal(deadline, 3000000004ULL, "relative sleep anchored to monotonic source");
        t.Equal(clock.CalculateSleepDeadline(0, true, request, &deadline), LINUX_ENOSYS, "absolute UTC sleep requires RTC");
        t.Equal(clock.CalculateSleepDeadline(4, false, request, &deadline), LINUX_EINVAL, "raw clock is not a sleep clock");

        fixture.realtime = true;
        fixture.seconds  = 1700000000;
        TimeManager utc({initialize_time, read_time, &fixture});
        request = {1700000002, 3};
        t.Equal(utc.CalculateSleepDeadline(0, true, request, &deadline), 0, "absolute realtime deadline conversion");
        t.Equal(deadline, 2000000003, "UTC offset removed exactly once");
        request = {1699999999, 999999999};
        t.Equal(utc.CalculateSleepDeadline(0, true, request, &deadline), 0, "past UTC deadline accepted");
        t.Equal(deadline, 0, "past UTC deadline immediately expires");
        fixture.now = UINT64_MAX - 1;
        request = {0, 2};
        t.Equal(clock.CalculateSleepDeadline(1, false, request, &deadline), LINUX_EOVERFLOW, "relative addition overflow rejected");
        fixture.available = false;
        t.Equal(clock.ReadClock(1, &value), LINUX_ENOSYS, "failed source read reported");
        t.Equal(clock.CalculateSleepDeadline(1, false, request, &deadline), LINUX_ENOSYS, "failed source cannot report successful sleep");
        TimeManager missing({initialize_time, read_time, &fixture});
        t.Equal(missing.ReadResolution(1, &value), LINUX_ENOSYS, "missing counters have no fabricated resolution");
        t.Equal(clock.ReadClock(2, &value), LINUX_EINVAL, "process CPU clock awaits live accounting snapshot");
        t.Equal(clock.ReadClock(3, &value), LINUX_EINVAL, "thread CPU clock awaits per-thread accounting");
        t.Equal(clock.ReadClock(7, &value), LINUX_EINVAL, "boottime awaits suspend accounting");
    });

    suite.Run("resource/TimeManager/fallback_alarm", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        time_backend_fixture_t fixture = {true, false, 1000000000, 0};
        TimeManager clock({initialize_time, read_time, &fixture});
        t.Equal(clock.SetAlarm(2), 0, "alarm arms without RTC");
        t.Equal(f.Process->realTimerDeadline, 3000000000, "alarm uses selected monotonic epoch");
        fixture.now = 2500000000;
        t.Equal(clock.CancelAlarm(), 1, "alarm cancellation rounds remaining interval");
        fixture.now = UINT64_MAX - 1;
        t.Equal(clock.SetAlarm(1), LINUX_EOVERFLOW, "alarm deadline overflow rejected");
        t.Check(!f.Process->realTimerArmed, "failed alarm leaves timer disarmed");
        fixture.available = false;
        t.Equal(clock.SetAlarm(1), LINUX_ENOSYS, "unavailable source cannot arm alarm");
        t.Equal(clock.CancelAlarm(), 0, "disarmed alarm cancellation needs no clock");
    });

    suite.Run("resource/TimeManager/cpu_expiration_and_limits", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        f.Process->cpuTimerDeadline[0] = 1000000;
        f.Process->cpuTimerDeadline[1] = 2000000;
        f.Process->cpuTimerInterval[0] = 1000000;
        f.Process->kernelCpuNanoseconds = 1999999;
        t.Check(!m->ExpireCpuTimers(f.Process), "virtual timer ignores kernel execution and prof does not expire early");
        f.Process->kernelCpuNanoseconds = 2000000;
        t.Check(m->ExpireCpuTimers(f.Process), "prof timer expires on kernel execution");
        t.Check((f.Process->signalPending & (1ULL << 26)) != 0 && (f.Process->signalPending & (1ULL << 25)) == 0, "SIGPROF and SIGVTALRM use distinct CPU clocks");
        t.Equal(f.Process->cpuTimerDeadline[1], 0, "one-shot profiling timer disarms");
        f.Process->userCpuNanoseconds = 5500000;
        t.Check(m->ExpireCpuTimers(f.Process), "virtual timer expires after user CPU execution");
        t.Equal(f.Process->cpuTimerDeadline[0], 6000000, "periodic CPU timer skips missed intervals without drift");
        t.Equal(f.Process->signalSenders[25].code, 128, "CPU timer publishes SI_KERNEL");
        __atomic_store_n(&f.Process->signalPending, 0ULL, __ATOMIC_RELEASE);
        f.Process->Signals()->ignored |= 1ULL << 25;
        f.Process->userCpuNanoseconds = 6000000;
        t.Check(!m->ExpireCpuTimers(f.Process), "ignored CPU timer signal is discarded");
        t.Equal(f.Process->cpuTimerDeadline[0], 7000000, "ignored periodic timer still rearms");
        f.Process->cpuTimerDeadline[0] = 0;
        f.Process->cpuLimit = {1, 3};
        f.Process->userCpuNanoseconds = 999999999;
        f.Process->kernelCpuNanoseconds = 0;
        t.Check(!m->ExpireCpuTimers(f.Process), "CPU soft limit does not fire early");
        f.Process->kernelCpuNanoseconds = 1;
        t.Check(m->ExpireCpuTimers(f.Process), "CPU soft limit combines user and kernel time");
        t.Check((f.Process->signalPending & (1ULL << 23)) != 0, "soft CPU limit publishes SIGXCPU");
        __atomic_store_n(&f.Process->signalPending, 0ULL, __ATOMIC_RELEASE);
        t.Check(!m->ExpireCpuTimers(f.Process), "soft limit does not repeatedly signal in the same CPU second");
        f.Process->userCpuNanoseconds = 2000000000;
        t.Check(m->ExpireCpuTimers(f.Process), "soft limit signals again after another CPU second");
        __atomic_store_n(&f.Process->signalPending, 0ULL, __ATOMIC_RELEASE);
        f.Process->userCpuNanoseconds = 3000000000;
        t.Check(m->ExpireCpuTimers(f.Process), "hard limit expires");
        t.Check((f.Process->signalPending & (1ULL << 8)) != 0, "hard CPU limit publishes unmaskable SIGKILL");
        __atomic_store_n(&f.Process->signalPending, 0ULL, __ATOMIC_RELEASE);
        f.Process->cpuLimit = {0, 0};
        f.Process->userCpuNanoseconds = 999999999;
        f.Process->kernelCpuNanoseconds = 0;
        t.Check(!m->ExpireCpuTimers(f.Process), "zero CPU limit retains Linux one-second minimum");
        f.Process->userCpuNanoseconds = 1000000000;
        t.Check(m->ExpireCpuTimers(f.Process), "zero hard CPU limit kills at one CPU second");
        __atomic_store_n(&f.Process->signalPending, 0ULL, __ATOMIC_RELEASE);
        f.Process->exited = true;
        t.Check(!m->ExpireCpuTimers(f.Process), "exited process cannot receive timer signals");
    });

    suite.Run("resource/TimeManager/invalid_clock_deadline", [&](TestCase& t)
    {
        time_timespec_t value    = {}, bad = { - 1, 0};
        uint64_t        deadline = 0;
        t.Equal(m->ReadClock(99, &value), LINUX_EINVAL, "invalid clock ID");
        t.Equal(m->ReadResolution(99, &value), LINUX_EINVAL, "invalid resolution clock");
        t.Equal(m->CalculateSleepDeadline(1, false, bad, &deadline), LINUX_EINVAL, "negative seconds");
        bad = {0, 1000000000};
        t.Equal(m->CalculateSleepDeadline(1, false, bad, &deadline), LINUX_EINVAL, "nanosecond boundary");
    });

    suite.Run("resource/TimeManager/monotonic_resolution", [&](TestCase& t)
    {
        time_timespec_t a      = {}, b = {}, resolution = {};
        uint64_t        result = m->ReadClock(1, &a);

        if (result == LINUX_ENOSYS)
        {
            t.Skip("monotonic clock unsupported");
            return;
        }

        if (!t.Equal(result, 0, "read monotonic clock"))
        {
            return;
        }

        t.Equal(m->ReadClock(1, &b), 0, "second clock sample");
        t.Check(b.seconds > a.seconds || (b.seconds == a.seconds && b.nanoseconds >= a.nanoseconds), "monotonic samples");
        t.Equal(m->ReadResolution(1, &resolution), 0, "clock resolution");
        t.Check(resolution.seconds > 0 || resolution.nanoseconds > 0, "positive resolution");
    });

    suite.Run("resource/TimeManager/timer_input_validation", [&](TestCase& t)
    {
        t.Equal(m->GetTimer(-1, 0), LINUX_EINVAL, "invalid timer index");
        t.Equal(m->SetTimer(3, 0, 0), LINUX_EINVAL, "invalid timer index for set");
    });

    suite.Run("resource/TimeManager/absolute_deadline_arithmetic", [&](TestCase& t)
    {
        time_timespec_t request  = {1, 234};
        uint64_t        deadline = 0;
        uint64_t        result   = m->CalculateSleepDeadline(1, true, request, &deadline);

        if (result == LINUX_ENOSYS)
        {
            t.Skip("monotonic clock unsupported");
            return;
        }

        t.Equal(result, 0, "absolute monotonic deadline");
        t.Equal(deadline, 1000000234, "seconds and nanoseconds conversion");
        request = {INT64_MAX, 999999999};
        t.Equal(m->CalculateSleepDeadline(1, true, request, &deadline), LINUX_EOVERFLOW, "deadline overflow rejected");
    });

    suite.Run("resource/TimeManager/alarm_expiration", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        uint64_t result = m->SetAlarm(1);

        if (result == LINUX_ENOSYS)
        {
            t.Skip("alarm clock unsupported");
            return;
        }

        if (!t.Equal(result, 0, "arm alarm without prior timer"))
        {
            return;
        }

        uint64_t deadline = f.Process->realTimerDeadline;
        t.Check(deadline > 0, "armed alarm has a deadline");
        t.Check(!m->ExpireRealTimer(f.Process, deadline - 1), "alarm does not expire early");
        t.Check(m->ExpireRealTimer(f.Process, deadline), "alarm expires at deadline");
        t.Check((f.Process->signalPending & (1ULL << 13)) != 0, "expiration publishes SIGALRM");
        t.Equal(m->CancelAlarm(), 0, "expired alarm cancellation has no remaining time");
    });
}
