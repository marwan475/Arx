#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/TimeManager.hpp"

void test_TimeManager(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->timeManager : nullptr;
    ManagerSuite suite("resource/TimeManager", m != nullptr);
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
