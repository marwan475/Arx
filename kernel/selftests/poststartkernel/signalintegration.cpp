#include "integrationtests.hpp"
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SignalManager.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/TimeManager.hpp"

#include <arch/selftest.hpp>
#include <selftests/TestFixture.hpp>

void test_signal_integration(ResourceLayerCaps* r, LogicLayerCaps* l)
{
    TestCase t("poststartkernel/signals/user_frame_roundtrip");
    if (!arch_task_context_supported())
    {
        t.Skip("architecture has no user signal contexts");
        return;
    }
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

    arch_selftest_floating_state_t floating = {};
    arch_syscall_frame_t          frame    = {};
    arch_selftest_user_frame(&frame, TestFixture::Buffer + 128, TestFixture::Buffer + PAGE_SIZE, &floating);
    signal_action_t action = {TestFixture::Buffer + 256, 0x04000000, TestFixture::Buffer + 512, 0};

    if (!t.Equal(l->signalManager->Action(10, &action, nullptr), 0, "install owned user action"))
    {
        return;
    }

    if (!t.Equal(l->signalManager->Send(f.Process->id, - 1, 10), 0, "queue process signal"))
    {
        return;
    }

    t.Equal(l->signalManager->Deliver(&frame, 0x9876), 0, "prepare handler return frame");
    t.Equal(arch_frame_instruction_pointer(&frame), action.handler, "delivery selects handler");
    t.Equal(frame.arg0, 10, "delivery supplies signal number");
    t.Equal(f.Task->signalDepth, 1, "signal frame owns one nesting level");
    arch_selftest_signal_restorer(&frame);
    // The handler's RET consumes the restorer address.
    t.Equal(l->signalManager->Restore(&frame), 0x9876, "restore interrupted result");
    t.Equal(arch_frame_instruction_pointer(&frame), TestFixture::Buffer + 128, "restore interrupted instruction pointer");
    t.Equal(arch_signal_stack_pointer(&frame), TestFixture::Buffer + PAGE_SIZE, "restore interrupted stack");
    t.Equal(f.Task->signalDepth, 0, "signal nesting restored");

    t.Equal(l->signalManager->Action(34, &action, nullptr), 0, "install real-time user handler");
    f.Task->signalMask = 1ULL << 33;
    t.Equal(l->signalManager->Send(f.Process->id, -1, 34), 0, "queue masked real-time handler record");
    t.Equal(l->signalManager->Send(f.Process->id, -1, 34), 0, "queue repeated real-time handler record");
    t.Equal(l->signalManager->Deliver(&frame, 88), 88, "masked real-time handler stays pending");
    t.Equal(f.Process->signalQueue->count, 2, "masked delivery preserves both records");
    f.Task->signalMask = 0;
    for (size_t index = 0; index < 2; ++index)
    {
        t.Equal(l->signalManager->Deliver(&frame, 88), 0, "deliver one repeated real-time handler");
        t.Equal(frame.arg0, 34, "real-time handler receives signal number");
        t.Equal(f.Process->signalQueue->count, 1 - index, "handler consumes exactly one real-time record");
        arch_selftest_signal_restorer(&frame);
        t.Equal(l->signalManager->Restore(&frame), 88, "real-time handler restores interrupted context");
    }
    t.Equal(f.Process->signalPending, 0, "repeated handlers clear final pending bit");

    size_t         instructionSize = 0;
    const uint8_t* instruction     = arch_selftest_syscall_instruction(&instructionSize);
    t.Check(safe_write_user(TestFixture::Buffer + 126, instruction, instructionSize), "write interrupted syscall instruction");
    action.flags |= 0x10000000;
    t.Equal(l->signalManager->Action(34, &action, nullptr), 0, "install restarting real-time handler");
    t.Equal(l->signalManager->Send(f.Process->id, -1, 34), 0, "queue restarting real-time signal");
    frame.syscall_number = 0;
    t.Equal(l->signalManager->Deliver(&frame, static_cast<uint64_t>(-4), true), 0, "real-time handler prepares restart context");
    arch_selftest_signal_restorer(&frame);
    t.Equal(l->signalManager->Restore(&frame), 0, "real-time restart restores syscall number");
    t.Equal(arch_frame_instruction_pointer(&frame), TestFixture::Buffer + 126, "real-time SA_RESTART rewinds syscall instruction");
    arch_selftest_set_instruction_pointer(&frame, TestFixture::Buffer + 128);
    action.flags &= ~0x10000000ULL;
    t.Equal(l->signalManager->Action(34, &action, nullptr), 0, "install non-restarting real-time handler");
    t.Equal(l->signalManager->Send(f.Process->id, -1, 34), 0, "queue non-restarting real-time signal");
    t.Equal(l->signalManager->Deliver(&frame, static_cast<uint64_t>(-4), true), 0, "real-time handler prepares interruption context");
    arch_selftest_signal_restorer(&frame);
    t.Equal(l->signalManager->Restore(&frame), static_cast<uint64_t>(-4), "real-time handler preserves EINTR without SA_RESTART");
    t.Equal(arch_frame_instruction_pointer(&frame), TestFixture::Buffer + 128, "non-restarting real-time handler preserves return instruction");

    const uint64_t descriptor = l->eventManager->SignalDescriptor(-1, 1ULL << 33, 04000);
    if (t.Check(static_cast<int64_t>(descriptor) >= 0, "create shared handler/signalfd consumption fixture"))
    {
        t.Equal(l->signalManager->Send(f.Process->id, -1, 34), 0, "queue handler-owned real-time record");
        t.Equal(l->signalManager->Send(f.Process->id, -1, 34), 0, "queue descriptor-owned real-time record");
        t.Equal(l->signalManager->Deliver(&frame, 99), 0, "handler consumes first shared pending record");
        arch_selftest_signal_restorer(&frame);
        t.Equal(l->signalManager->Restore(&frame), 99, "return from shared-consumption handler");
        file_t* file = static_cast<file_t*>(f.Process->Files()->entries[descriptor].file);
        t.Equal(file->operations->ReadUser(file, TestFixture::Buffer, 128), 128, "signalfd consumes remaining shared pending record");
        t.Equal(file->operations->ReadUser(file, TestFixture::Buffer, 128), LINUX_EAGAIN, "handler and signalfd do not duplicate queued records");
        t.Equal(l->signalManager->Deliver(&frame, 99), 99, "signalfd consumption leaves no handler record");
    }

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
        t.Equal(arch_frame_instruction_pointer(&frame), action.handler, "CPU expiration selects installed handler");
        arch_selftest_signal_restorer(&frame);
        t.Equal(l->signalManager->Restore(&frame), 77, "CPU handler restores interrupted result");
        t.Equal(f.Task->signalDepth, 0, "CPU handler restores signal nesting");
    }
}
