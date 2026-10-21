#include <selftests/TestFile.hpp>
#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"

void test_EventManager(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->eventManager : nullptr;
    ManagerSuite suite("logic/EventManager", m != nullptr);

#if DEBUG
    suite.Run("logic/EventManager/debug_notification_accounting", [&](TestCase& t)
    {
        const EventDebugStats before = m->DebugStats;

        m->Notify();
        t.Equal(m->CreateCounter(0, 0x80000000), LINUX_EINVAL, "reject invalid counter flags");
        t.Equal(m->CreateEpoll(0x80000000), LINUX_EINVAL, "reject invalid epoll flags");

        t.Equal(m->DebugStats.notifications - before.notifications, 1, "general notification counts an attempt");
        t.Equal(m->DebugStats.createCounter.failures - before.createCounter.failures, 1, "record failed counter creation");
        t.Equal(m->DebugStats.createEpoll.failures - before.createEpoll.failures, 1, "record failed epoll creation");
        t.Equal(m->DebugStats.liveCounters, before.liveCounters, "failed creation adds no live object");
        t.Equal(m->DebugStats.epollCopied, before.epollCopied, "notification is not user event delivery");
        t.Equal(m->DebugStats.inotifyCopied, before.inotifyCopied, "notification copies no inotify record");
        t.Equal(m->DebugStats.signalfdCopied, before.signalfdCopied, "notification consumes no signal record");
    });
#endif

    suite.Run("logic/EventManager/notification_sequence", [&](TestCase& t)
    {
        uint64_t sequence = m->Sequence();
        m->Notify();
        t.Check(m->Sequence() != sequence, "notification publishes generation");
    });

    suite.Run("logic/EventManager/counter_nonblocking", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t) || !t.Check(f.Map(), "map user counter buffer"))
        {
            return;
        }

        int64_t fd = m->CreateCounter(7, 04000);

        if (!t.Check(fd >= 0, "create nonblocking counter"))
        {
            return;
        }

        TestFile retained(t, resources, logic->virtualFileSystem, f.Process, fd);
        auto* file = retained.Get();

        if (!file)
        {
            return;
        }
        t.Equal(file->operations->ReadUser(file, TestFixture::Buffer, 8), 8, "counter read size");
        uint64_t value = 0;
        t.Check(safe_read_user(TestFixture::Buffer, &value, 8), "copy counter result");
        t.Equal(value, 7, "counter read drains initial value");
        t.Equal(file->operations->ReadUser(file, TestFixture::Buffer, 8), LINUX_EAGAIN, "empty nonblocking counter");
        t.Check(f.Close(fd), "close counter");
    });

    suite.Run("logic/EventManager/epoll_membership", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        int64_t epoll = m->CreateEpoll(0), counter = m->CreateCounter(0, 04000);

        if (!t.Check(epoll >= 0 && counter >= 0, "create epoll and source"))
        {
            return;
        }

        event_epoll_event_t event = {EVENT_POLL_IN, 0x1234};
        t.Equal(m->ControlEpoll(epoll, 1, counter, &event), 0, "add watch");
        t.Equal(m->ControlEpoll(epoll, 1, counter, &event), (uint64_t) -17, "duplicate watch rejected");
        t.Equal(m->ControlEpoll(epoll, 2, counter, nullptr), 0, "delete watch");
        t.Equal(m->ControlEpoll(epoll, 2, counter, nullptr), LINUX_ENOENT, "missing watch rejected");
    });

    suite.Run("logic/EventManager/flags_validation", [&](TestCase& t)
    {
        t.Equal(m->CreateCounter(0, 0x80000000), LINUX_EINVAL, "unknown counter flags");
        t.Equal(m->CreateEpoll(0x80000000), LINUX_EINVAL, "unknown epoll flags");
    });
}
