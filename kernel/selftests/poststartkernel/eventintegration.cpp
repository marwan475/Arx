#include <selftests/TestFile.hpp>
#include "integrationtests.hpp"

#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/TimeManager.hpp"

void test_event_integration(ResourceLayerCaps* r, LogicLayerCaps* l)
{
    {
        TestCase t("poststartkernel/events/counter_epoll_notification");

        if (!l || !l->eventManager)
        {
            t.Block("event manager unavailable");
            return;
        }

        TestFixture f(r);

        if (!f.Valid(t) || !t.Check(f.Map(), "map event output"))
        {
            return;
        }

        int64_t ep = l->eventManager->CreateEpoll(0), fd = l->eventManager->CreateCounter(0, 04000);

        if (!t.Check(ep >= 0 && fd >= 0, "create event source and observer"))
        {
            return;
        }

        event_epoll_event_t event = {EVENT_POLL_IN, 0x5544}, actual = {};

        if (!t.Equal(l->eventManager->ControlEpoll(ep, 1, fd, &event), 0, "attach counter readiness watch"))
        {
            return;
        }

        t.Equal(l->eventManager->WaitEpoll(ep, TestFixture::Buffer + 64, 1, 0), 0, "empty source is not readable");
        uint64_t value = 5;
        t.Check(safe_write_user(TestFixture::Buffer, &value, 8), "seed counter write");
        TestFile retained(t, r, l->virtualFileSystem, f.Process, fd);
        auto* source = retained.Get();

        if (!source)
        {
            return;
        }
        t.Equal(source->operations->WriteUser(source, TestFixture::Buffer, 8), 8, "publish source state");
        t.Equal(l->eventManager->WaitEpoll(ep, TestFixture::Buffer + 64, 1, 0), 1, "notification reaches epoll observer");
        t.Check(safe_read_user(TestFixture::Buffer + 64, &actual, sizeof(actual)), "copy readiness event");
        t.Equal(actual.data, event.data, "epoll preserves registration payload");
        t.Check((actual.events & EVENT_POLL_IN) != 0, "event contains readable bit");
        t.Equal(source->operations->ReadUser(source, TestFixture::Buffer, 8), 8, "drain counter");
        t.Equal(l->eventManager->WaitEpoll(ep, TestFixture::Buffer + 64, 1, 0), 0, "draining removes level readiness");
    }

    {
        TestCase t("poststartkernel/timers/scheduled_sleep");

        if (!l || !l->scheduler)
        {
            t.Block("scheduler unavailable");
            return;
        }

        uint64_t start = 0;

        uint64_t end = 0;

        if (!arch_time_read_monotonic(&start))
        {
            t.Skip("monotonic timer unavailable");
            return;
        }

        uint64_t deadline = start + 1000000;
        // One millisecond, no unbounded sleep.
        if (!t.Equal(l->scheduler->SleepUntil(deadline), 0, "scheduled sleep resumes"))
        {
            return;
        }

        t.Check(arch_time_read_monotonic(&end) && end >= deadline, "timer wake respects deadline");
    }
}
