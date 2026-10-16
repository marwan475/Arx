#include "integrationtests.hpp"
#include "ScheduledFixture.hpp"
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/SynchronizationManager.hpp"

static void futex_waiter(ScheduledFixture& f)
{
    uint32_t word = 7;
    f.T.Check(safe_write_user(TestFixture::Buffer, &word, 4), "initialize shared futex word");

    // Keep the same-CPU peer from running before the waiter is parked. The
    // scheduler transfers this guard with the suspended task during Wait.
    cpu_preemption_disable();

    if (!f.PublishPeer())
    {
        cpu_preemption_enable();
        return;
    }

    f.Result = f.L->synchronizationManager->Wait(TestFixture::Buffer, 7, UINT32_MAX, f.Deadline);
    f.T.Equal(f.Result, 0, "queued futex waiter was woken");
    f.T.Check(f.PeerDone, "waker completed before waiter resumed");
    cpu_preemption_enable();
}

static void futex_waker(ScheduledFixture& f)
{
    uint64_t count = f.L->synchronizationManager->Wake(TestFixture::Buffer, 1, UINT32_MAX);
    f.Word         = static_cast<uint32_t>(count);
}

static void event_waiter(ScheduledFixture& f)
{
    uint64_t sequence = f.L->eventManager->Sequence();
    cpu_preemption_disable();

    if (!f.PublishPeer())
    {
        cpu_preemption_enable();
        return;
    }

    f.Result = f.L->eventManager->Wait(sequence, f.Deadline);
    f.T.Equal(f.Result, 0, "event waiter resumed after notification");
    f.T.Check(f.L->eventManager->Sequence() != sequence, "wait observes published event generation");
    f.T.Check(f.PeerDone, "notifier completed before waiter resumed");
    cpu_preemption_enable();
}

static void event_notifier(ScheduledFixture& f)
{
    f.L->eventManager->Notify();
}

void test_wait_integration(ResourceLayerCaps* r, LogicLayerCaps* l)
{
    {
        TestCase t("poststartkernel/synchronization/scheduled_wait_wake");

        if (!r || !l || !l->scheduler || !l->synchronizationManager)
        {
            t.Block("scheduler or synchronization unavailable");
            return;
        }

        uint64_t now;

        if (!arch_time_read_monotonic(&now))
        {
            t.Skip("bounded wait requires monotonic clock");
        }
        else
        {
            ScheduledFixture f(t, r, l, futex_waiter, futex_waker);
            f.Run(now + 1000000000);
            t.Equal(f.Word, 1, "exactly one futex waiter was removed");
        }
    }

    {
        TestCase t("poststartkernel/events/scheduled_wait_notify");

        if (!r || !l || !l->scheduler || !l->eventManager)
        {
            t.Block("scheduler or event manager unavailable");
            return;
        }

        uint64_t now;

        if (!arch_time_read_monotonic(&now))
        {
            t.Skip("bounded wait requires monotonic clock");
            return;
        }

        ScheduledFixture f(t, r, l, event_waiter, event_notifier);
        f.Run(now + 1000000000);
    }
}
