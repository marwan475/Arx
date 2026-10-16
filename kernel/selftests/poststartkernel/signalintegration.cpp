#include "integrationtests.hpp"

#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SignalManager.hpp"
#include "layers/Resource/TimeManager.hpp"

void test_signal_integration(ResourceLayerCaps* r, LogicLayerCaps* l)
{
    TestCase t("poststartkernel/signals/user_frame_roundtrip");
#if !defined(__x86_64__)
    (void) r;
    (void) l;
    t.Skip("architecture has no user signal contexts");
#else
    if (!l || !l->signalManager)
    {
        t.Block("signal manager unavailable");
        return;
    }

    TestFixture f(r);

    if (!f.Valid(t) || !t.Check(f.Map(), "map signal frame page"))
    {
        return;
    }

    alignas(16) uint8_t floating[512] = {};
    arch_syscall_frame_t frame = {};
    frame.fxstate              = reinterpret_cast<uint64_t>(floating);
    frame.user_rip             = TestFixture::Buffer + 128;
    frame.user_rsp             = TestFixture::Buffer + PAGE_SIZE;
    frame.user_cs              = USER_CS;
    frame.user_ss              = USER_SS;
    frame.user_rflags          = 0x202;
    signal_action_t action     = {TestFixture::Buffer + 256, 0x04000000, TestFixture::Buffer + 512, 0};

    if (!t.Equal(l->signalManager->Action(10, &action, nullptr), 0, "install owned user action"))
    {
        return;
    }

    if (!t.Equal(l->signalManager->Send(f.Process->id, - 1, 10), 0, "queue process signal"))
    {
        return;
    }

    t.Equal(l->signalManager->Deliver(&frame, 0x9876), 0, "prepare handler return frame");
    t.Equal(frame.user_rip, action.handler, "delivery selects handler");
    t.Equal(frame.arg0, 10, "delivery supplies signal number");
    t.Equal(f.Task->signalDepth, 1, "signal frame owns one nesting level");
    frame.user_rsp += 8;
    // The handler's RET consumes the restorer address.
    t.Equal(l->signalManager->Restore(&frame), 0x9876, "restore interrupted result");
    t.Equal(frame.user_rip, TestFixture::Buffer + 128, "restore interrupted instruction pointer");
    t.Equal(frame.user_rsp, TestFixture::Buffer + PAGE_SIZE, "restore interrupted stack");
    t.Equal(f.Task->signalDepth, 0, "signal nesting restored");

    const int cpuSignals[] = {26, 27, 24};

    for (int signal : cpuSignals)
    {
        const uint64_t bit = 1ULL << (signal - 1);
        t.Equal(l->signalManager->Action(signal, &action, nullptr), 0, "install CPU expiration handler");
        f.Task->signalMask = bit;
        f.Process->userCpuNanoseconds = signal == 26 ? 1000000 : 0;
        f.Process->kernelCpuNanoseconds = signal == 27 ? 1000000 : signal == 24 ? 1000000000 : 0;

        if (signal == 24)
        {
            f.Process->cpuLimit = {1, UINT64_MAX};
        }
        else
        {
            f.Process->cpuTimerDeadline[signal - 26] = 1000000;
        }

        t.Check(r->timeManager->ExpireCpuTimers(f.Process), "CPU accounting publishes expiration signal");
        t.Equal(l->signalManager->Deliver(&frame, 77), 77, "blocked CPU signal preserves interrupted context");
        t.Check((f.Process->signalPending & bit) != 0 && f.Task->signalDepth == 0, "blocked CPU signal stays pending");
        f.Task->signalMask = 0;
        t.Equal(l->signalManager->Deliver(&frame, 77), 0, "unblocked CPU signal prepares user handler");
        t.Equal(frame.arg0, signal, "CPU handler receives SIGVTALRM, SIGPROF or SIGXCPU");
        t.Equal(frame.user_rip, action.handler, "CPU expiration selects installed handler");
        frame.user_rsp += 8;
        t.Equal(l->signalManager->Restore(&frame), 77, "CPU handler restores interrupted result");
        t.Equal(f.Task->signalDepth, 0, "CPU handler restores signal nesting");
    }
#endif
}
