#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SignalManager.hpp"

void test_SignalManager(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->signalManager : nullptr;
    ManagerSuite suite("logic/SignalManager", m != nullptr);

#if DEBUG
    suite.Run("logic/SignalManager/debug_queue_accounting", [&](TestCase& t)
    {
        TestFixture fixture(resources);

        if (!fixture.Valid(t))
        {
            return;
        }

        const signal_action_t ignoredAction = {1, 0, 0, 0};
        t.Equal(m->Action(10, &ignoredAction, nullptr), 0, "prepare ignored submission");

        SignalDispositionGuard guard(fixture.Process);
        const signal_sender_t sender = {11, 22, -1, 99};
        const SignalDebugStats before = m->DebugStats;

        t.Equal(m->Queue(fixture.Process, nullptr, 1, sender), 0, "accept standard signal");
        t.Equal(m->Queue(fixture.Process, nullptr, 1, sender), 0, "coalesce standard signal");
        t.Equal(m->Queue(fixture.Process, nullptr, 32, sender), 0, "append RT record");
        t.Equal(m->Queue(fixture.Process, nullptr, 10, sender), 0, "ignore successful submission");
        t.Equal(m->Queue(fixture.Process, nullptr, 0, sender), LINUX_EINVAL, "reject invalid signal");

        t.Equal(m->DebugStats.queue.attempts - before.queue.attempts, 5, "count all queue attempts");
        t.Equal(m->DebugStats.queue.successes - before.queue.successes, 4, "ignored and coalesced calls succeed");
        t.Equal(m->DebugStats.queue.failures - before.queue.failures, 1, "record rejected queue call");
        t.Equal(m->DebugStats.standardCoalesced - before.standardCoalesced, 1, "coalescing appends no record");
        t.Equal(m->DebugStats.ignored - before.ignored, 1, "ignored is distinct from queued");
        t.Equal(m->DebugStats.realtimeQueued - before.realtimeQueued, 1, "only RT append counts a record");

        m->Consume(fixture.Process, fixture.Task, 1);
        m->Consume(fixture.Process, fixture.Task, 32);
        t.Equal(m->DebugStats.consumed - before.consumed, 2, "count signal consumption");
        t.Equal(m->DebugStats.realtimeConsumed - before.realtimeConsumed, 1, "count RT record consumption");
        t.Equal(m->DebugStats.handlerFrames - before.handlerFrames, 0, "queue and consume do not imply handler delivery");
    });
#endif

    suite.Run("logic/SignalManager/action_roundtrip", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        signal_action_t ignored = {1, 0, 0, UINT64_MAX}, actual = {};
        t.Equal(m->Action(10, &ignored, nullptr), 0, "set ignored action");
        t.Equal(m->Action(10, nullptr, &actual), 0, "query action");
        t.Equal(actual.handler, 1, "handler round trip");
        t.Equal(actual.mask & SIGNAL_UNBLOCKABLE, 0, "uncatchable signals stripped from action mask");
        t.Equal(m->Action(9, &ignored, nullptr), LINUX_EINVAL, "SIGKILL handler rejected");
        t.Equal(m->Action(0, nullptr, &actual), LINUX_EINVAL, "invalid signal rejected");
    });

    suite.Run("logic/SignalManager/mask_temporary_restore", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        uint64_t all = UINT64_MAX, previous = 0, actual = 0;
        t.Equal(m->Mask(2, &all, &previous), 0, "replace blocked mask");
        t.Equal(m->Mask(0, nullptr, &actual), 0, "query mask");
        t.Equal(actual, all & ~SIGNAL_UNBLOCKABLE, "uncatchable signals never masked");
        t.Equal(m->BeginTemporaryMask(0), 0, "begin temporary mask");
        m->FinishTemporaryMask(false);
        t.Equal(m->Mask(0, nullptr, &actual), 0, "query restored mask");
        t.Equal(actual, all & ~SIGNAL_UNBLOCKABLE, "temporary mask restores prior state");
    });

    suite.Run("logic/SignalManager/realtime_queue",
              [&](TestCase& t)
              {
                  TestFixture f(resources);

                  if (!f.Valid(t))
                  {
                      return;
                  }

                  SignalDispositionGuard guard(f.Process);
                  signal_sender_t        first = {11, 22, -1, 0x1122334455667788ULL}, second = {33, 44, -1, 0x8877665544332211ULL}, actual = {};

                  t.Equal(m->Queue(f.Process, nullptr, 64, first), 0, "highest RT number is supported");
                  t.Equal(m->Queue(f.Process, nullptr, 32, first), 0, "lowest RT number is supported");
                  t.Equal(m->Queue(f.Process, nullptr, 32, second), 0, "repeated RT number queues another record");

                  t.Equal(m->Peek(f.Process, f.Task, UINT64_MAX, &actual), 32, "RT delivery selects lowest number");
                  t.Equal(actual.pid, first.pid, "first sender retained");
                  t.Equal(actual.value, first.value, "first full-width payload retained");

                  m->Consume(f.Process, f.Task, 32);
                  t.Equal(m->Peek(f.Process, f.Task, UINT64_MAX, &actual), 32, "repeated RT number remains pending");
                  t.Equal(actual.pid, second.pid, "FIFO sender ordering within number");
                  t.Equal(actual.value, second.value, "repeated signal payload ordering");

                  m->Consume(f.Process, f.Task, 32);
                  t.Equal(m->Peek(f.Process, f.Task, 1ULL << 31, &actual), 0, "mask excludes other queued numbers");
                  m->Consume(f.Process, f.Task, 64);

                  for (size_t i = 0; i < signal_queue_t::CAPACITY; ++i)
                  {
                      t.Equal(m->Queue(f.Process, f.Task, 34, first), 0, "bounded target queue accepts each record");
                  }

                  t.Equal(m->Queue(f.Process, f.Task, 34, second), LINUX_EAGAIN, "queue exhaustion explicitly rejects additional send");
                  t.Equal(f.Task->signalQueue->count, signal_queue_t::CAPACITY, "exhaustion preserves earlier records");
                  t.Equal(m->Queue(f.Process, nullptr, 35, second), 0, "process and thread have separate queue budgets");
                  t.Equal(m->Peek(f.Process, f.Task, UINT64_MAX, &actual), 34, "thread RT queue precedes process RT queue");

                  for (size_t i = 0; i < signal_queue_t::CAPACITY; ++i)
                  {
                      m->Consume(f.Process, f.Task, 34);
                  }

                  t.Equal(f.Task->signalPending, 0, "last queued record clears summary bit");

                  m->Discard(&f.Process->signalPending, f.Process->signalQueue, 35);
                  t.Equal(f.Process->signalQueue->count, 0, "discard releases queued record capacity");
              });

    suite.Run("logic/SignalManager/reserved_exit_delivery", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        auto space = resources->virtualMemoryManager->CreateUserAddressSpace();
        process_t* child = space != nullptr ? resources->processManager->CreateProcess(space) : nullptr;

        if (!t.Check(child != nullptr, "allocate reserved exit fixture"))
        {
            if (space != nullptr)
            {
                resources->virtualMemoryManager->DestroyUserAddressSpace(space);
            }

            return;
        }

        child->exitSignal = 34;
        child->exitStatus = 23;
        t.Equal(m->ReserveExit(child, f.Process), 0, "reserve exit capacity before clone publication");

        {
            SignalDispositionGuard guard(f.Process);
            const signal_sender_t sender = {11, 22, -1, 99};

            for (size_t i = 0; i < signal_queue_t::CAPACITY - 1; ++i)
            {
                t.Equal(m->Queue(f.Process, nullptr, 35, sender), 0, "ordinary send uses unreserved capacity");
            }

            t.Equal(m->Queue(f.Process, nullptr, 35, sender), LINUX_EAGAIN, "send cannot consume reserved exit slot");
        }

        m->ReleaseExit(resources, child, true);
        t.Check(resources->processManager->FreeProcess(child), "reap child before consuming exit notification");
        resources->virtualMemoryManager->DestroyUserAddressSpace(space);

        {
            SignalDispositionGuard guard(f.Process);
            signal_sender_t actual = {};
            t.Equal(f.Process->signalQueue->count, signal_queue_t::CAPACITY, "exit used reservation despite exhausted send capacity");
            t.Equal(m->Peek(f.Process, f.Task, 1ULL << 33, &actual), 34, "exit record survives child reap");
            t.Equal(actual.value, 23, "queued child status retained");
            m->Consume(f.Process, f.Task, 34);
            m->Discard(&f.Process->signalPending, f.Process->signalQueue, 35);
        }
    });

    suite.Run("logic/SignalManager/alternate_stack", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        signal_stack_t disabled = {0, 2, 0, 0}, actual = {}, shortStack = {0x40000000, 0, 0, 1};
        t.Equal(m->Stack(&disabled, &actual, 0), 0, "disable alternate stack");
        t.Equal(m->Stack(nullptr, &actual, 0), 0, "query alternate stack");
        t.Equal(actual.flags, 2, "disabled state");
        t.Equal(m->Stack(&shortStack, nullptr, 0), LINUX_ENOMEM, "undersized stack rejected");
    });
}
