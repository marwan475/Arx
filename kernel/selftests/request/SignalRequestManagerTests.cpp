#include "RequestTestSupport.hpp"
#include "SignalAbiFixture.hpp"

namespace request_selftest
{
struct signal_peer_context_t
{
    ResourceLayerCaps* resources;
    Scheduler*         scheduler;
    process_t*         process;
    uint64_t           robustHead;
    uint64_t           result;
};

static void signal_peer_entry(void* argument)
{
    cpu_preemption_disable();

    auto context = static_cast<signal_peer_context_t*>(argument);

    context->result = context->robustHead != 0 ? syscall_call(SYSCALL_set_robust_list, context->robustHead, 24) : syscall_call(SYSCALL_kill, context->process->id, 10);

    context->scheduler->ExitCurrentTask();
}

template <typename Wait>
static bool signal_with_peer(process_t* process, uint64_t robustHead, uint64_t robustWord, Wait wait)
{
    Dispatcher*           dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto                  resources  = dispatcher->GetResourceLayerCaps();
    auto                  scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    signal_peer_context_t context    = {resources, scheduler, process, robustHead, UINT64_MAX};
    task_t*               peer       = resources->taskManager->CreateKernelTask(signal_peer_entry, &context);
    const bool            retained   = resources->taskManager->RetainForDispatch(peer);
    bool                  added      = retained && resources->processManager->AddTask(process, peer);

    if (added && robustWord != 0)
    {
        uint32_t owned = 0x80000000U | (uint32_t) peer->id;

        fixture_store(process, robustWord, &owned, 4);
    }

    const bool ready = added && scheduler->EnqueueProcess(arch_cpu_id(), process->id);

    bool ok = expect(ready, "signal/owner-death runnable peer fixture");

    if (ready)
    {
        ok &= wait(peer->id) && expect(context.result == 0, "signal/robust peer syscall succeeds");
    }

    if (added)
    {
        if (peer->prev != nullptr)
        {
            peer->prev->next = peer->next;
        }
        else
        {
            process->tasks = peer->next;
        }

        if (peer->next != nullptr)
        {
            peer->next->prev = peer->prev;
        }

        peer->next = peer->prev = nullptr;
    }

    if (peer != nullptr)
    {
        if (retained)
        {
            resources->taskManager->ReleaseDispatchClaim(peer);
        }

        ok &= cleanup(resources->taskManager->FreeTask(peer), "signal peer retirement cleanup");
    }

    return ok;
}

static bool test_signal_case(process_t* process, uint64_t number)
{
    const uint64_t base = fixture_map(6);

    if ((int64_t) base < 0)
    {
        return false;
    }

    auto           dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto           signals    = dispatcher->GetLogicLayerCaps()->signalManager;
    auto           resources  = dispatcher->GetResourceLayerCaps();
    const uint64_t input = base + PAGE_SIZE - 4, output = base + 128;

    const uint64_t tid = syscall_call(SYSCALL_gettid);

    bool            ok     = true;
    signal_action_t action = {base + 512, 0x14000004, base + 768, 0}; // RESTORER|RESTART|SIGINFO
    fixture_store(process, input, &action, sizeof(action));

    ok &= expect(syscall_call(SYSCALL_rt_sigaction, 10, input, 0, 8) == 0, "signal handler action install");

    if (number == SYSCALL_rt_sigaction)
    {
        signal_action_t queried = {};

        ok &= expect(syscall_call(number, 10, 0, output, 8) == 0, "signal action query");

        fixture_load(process, output, &queried, sizeof(queried));

        ok &= expect(queried.handler == action.handler && queried.flags == action.flags, "signal raw action ABI round trip");

        ok &= expect(syscall_call(number, 0, 0, output, 8) == LINUX_EINVAL && syscall_call(number, 9, input, 0, 8) == LINUX_EINVAL &&
                         syscall_call(number, 19, input, 0, 8) == LINUX_EINVAL && syscall_call(number, 10, input, 0, 16) == LINUX_EINVAL,
            "signal number/unblockable/size validation");

        ok &= expect(syscall_call(number, 10, CANONICAL_KERNEL_BASE, 0, 8) == LINUX_EFAULT, "signal action bad user input");

        action.flags |= 2;

        fixture_store(process, input, &action, sizeof(action));

        ok &= expect(syscall_call(number, 17, input, 0, 8) == 0 && syscall_call(number, 17, 0, output, 8) == 0, "child-reaping action flag accepted");

        fixture_load(process, output, &queried, sizeof(queried));

        ok &= expect(queried.flags == action.flags, "child-reaping action flag round trip");

        action.flags |= 8; // Unknown action flag.
        fixture_store(process, input, &action, sizeof(action));

        ok &= expect(syscall_call(number, 10, input, 0, 8) == LINUX_EOPNOTSUPP, "unknown signal action flag rejected");
    }
    else if (number == SYSCALL_rt_sigprocmask)
    {
        uint64_t mask = UINT64_MAX, queried = 0;

        fixture_store(process, input, &mask, 8);

        ok &= expect(syscall_call(number, 2, input, output, 8) == 0, "signal mask replacement");

        ok &= expect(syscall_call(number, 99, 0, output, 8) == 0, "signal mask query ignores how without input");

        fixture_load(process, output, &queried, 8);

        ok &= expect(queried == (UINT64_MAX & ~SIGNAL_UNBLOCKABLE), "SIGKILL/SIGSTOP cannot be masked");

        ok &= expect(syscall_call(number, 3, input, 0, 8) == LINUX_EINVAL && syscall_call(number, 0, input, 0, 4) == LINUX_EINVAL, "signal mask operation and size errors");

        mask = 0;

        fixture_store(process, input, &mask, 8);

        syscall_call(number, 2, input, 0, 8);
    }
    else if (number == SYSCALL_sigaltstack)
    {
        signal_stack_t stack = {base + PAGE_SIZE * 2, 0, 0, PAGE_SIZE * 2}, queried = {};

        fixture_store(process, input, &stack, sizeof(stack));

        ok &= expect(syscall_call(number, input, output) == 0 && syscall_call(number, 0, output) == 0, "alternate stack install/query");

        fixture_load(process, output, &queried, sizeof(queried));

        ok &= expect(queried.sp == stack.sp && queried.size == stack.size && queried.flags == 0, "alternate stack raw ABI");

        arch_syscall_frame_t active = {};

        active.syscall_number = number;
        active.arg0           = input;
        arch_frame_set_stack_pointer(&active, stack.sp + 64);
        ok &= expect(dispatcher_dispatch_syscall(&active) == (uint64_t) -1, "alternate stack cannot change while active");

        stack.size = 1024;

        fixture_store(process, input, &stack, sizeof(stack));

        ok &= expect(syscall_call(number, input) == LINUX_ENOMEM, "alternate stack minimum size");
    }
    else if (number == SYSCALL_kill || number == SYSCALL_tgkill)
    {
        auto send = [&](uint64_t signal)
        {
            return number == SYSCALL_kill ? syscall_call(number, process->id, signal) : syscall_call(number, process->id, tid, signal);
        };
        ok &= expect(send(0) == 0 && send(10) == 0, "signal existence probe and pending enqueue");

        task_t* task = resources->taskManager->GetCurrentTask();

        ok &= expect((pending_signals(process, task) & 512) != 0, "signal routed to current process/thread");

        ok &= expect(send(65) == LINUX_EINVAL && send(34) == 0 && send(34) == 0, "invalid signal rejected and repeated realtime signals queued");

        ok &= expect(number == SYSCALL_kill ? syscall_call(number, 0, 0) == 0 : syscall_call(number, process->id, 0, 10) == LINUX_EINVAL,
            "signal group lookup and invalid thread targeting");

        signal_action_t rtIgnore = {1, 0, 0, 0};
        fixture_store(process, input, &rtIgnore, sizeof(rtIgnore));
        ok &= expect(syscall_call(SYSCALL_rt_sigaction, 34, input, 0, 8) == 0 && ((process->signalPending | task->signalPending) & (1ULL << 33)) == 0, "RT ignore registration discards all queued instances");
        signal_action_t ignore = {1, 0, 0, 0};

        fixture_store(process, input, &ignore, sizeof(ignore));

        ok &= expect(syscall_call(SYSCALL_rt_sigaction, 10, input, 0, 8) == 0 && send(10) == 0 && (pending_signals(process, task) & 512) == 0,
            "ignored signal discards pending instances");

        auto       space  = resources->virtualMemoryManager->CreateUserAddressSpace();
        process_t* victim = space != nullptr ? resources->processManager->CreateProcess(space) : nullptr;

        if (victim != nullptr)
        {
            process_credentials_t previous = process->credentials;

            process->credentials.user = {100, 100, 100, 100};
            victim->credentials.user  = {200, 200, 200, 200};

            ok &= expect(syscall_call(SYSCALL_kill, victim->id, 0) == (uint64_t) -1, "signal permissions reject unrelated identities");

            process->credentials.user.effective = 0;

            ok &= expect(syscall_call(SYSCALL_kill, victim->id, 0) == 0, "root signal permission probe");

            process->credentials = previous;

            ok &= cleanup(resources->processManager->FreeProcess(victim), "signal permission target cleanup");
        }
        else
        {
            ok &= expect(false, "signal permission target fixture");
        }

        if (space != nullptr)
        {
            resources->virtualMemoryManager->DestroyUserAddressSpace(space);
        }
    }
    else if (number == SYSCALL_pause)
    {
        ok &= signal_with_peer(process, 0, 0, [&](uint64_t)
        {
            return expect(syscall_call(number) == (uint64_t) -4, "pause returns EINTR from real peer signal");
        });
        __atomic_store_n(&process->signalPending, 0ULL, __ATOMIC_RELEASE);

        time_timespec_t now;

        if (resources->timeManager->ReadClock(1, &now) == 0)
        {
            time_timespec_t duration = {1, 0}, remaining = {};

            fixture_store(process, input, &duration, sizeof(duration));

            ok &=
                signal_with_peer(process, 0, 0, [&](uint64_t)
            {
                return expect(syscall_call(SYSCALL_nanosleep, input, output) == (uint64_t) -4, "nanosleep signal interruption");
            });
            fixture_load(process, output, &remaining, sizeof(remaining));

            ok &= expect(remaining.seconds >= 0 && remaining.seconds <= 1 && remaining.nanoseconds >= 0 && remaining.nanoseconds < 1000000000,
                "interrupted sleep normalized remainder");
        }
    }
    else if (number == SYSCALL_rt_sigreturn)
    {
        if (arch_task_context_supported())
        {
            ok &= arch_selftest_signal_return(process, resources, signals, action, base, input);
            uint32_t word = 7;

            fixture_store(process, base + 64, &word, 4);

            ok &= signal_with_peer(process, 0, 0, [&](uint64_t) { return expect(syscall_call(SYSCALL_futex, base + 64, 128, 7) == (uint64_t) -4, "futex interrupted by real pending signal"); });
            ok &= expect(syscall_call(SYSCALL_futex, base + 64, 129, 1) == 0, "interrupted futex dequeues waiter");

            __atomic_store_n(&process->signalPending, 0ULL, __ATOMIC_RELEASE);

            struct
            {
                uint64_t next;
                int64_t  offset;
                uint64_t pending;
            } robust = {base + 256, 8, 0};

            uint64_t next = base + 128;

            fixture_store(process, base + 128, &robust, sizeof(robust));

            fixture_store(process, base + 256, &next, 8);

            ok &= signal_with_peer(process, base + 128, base + 264, [&](uint64_t owner) { return expect(syscall_call(SYSCALL_futex, base + 264, 128, 0x80000000U | (uint32_t) owner) == 0, "robust task exit wakes futex waiter"); });
            uint32_t died = 0;

            fixture_load(process, base + 264, &died, 4);

            ok &= expect(died == 0xc0000000U, "robust exit publishes OWNER_DIED with WAITERS preserved");
        }
    }

    return ok;
}

template <uint64_t Number>
static bool validate_signal(const syscall_case_t*)
{
    return with_process([](process_t* process)
    {
        auto           resources = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps();
        task_t*        task      = resources->taskManager->GetCurrentTask();
        uint64_t       mask = task->signalMask, pending = task->signalPending;
        signal_stack_t stack = task->signalStack;
        uint64_t       depth = task->signalDepth;
        bool           ok    = test_signal_case(process, Number);

        task->signalMask    = mask;
        task->signalPending = pending;
        task->signalStack   = stack;
        task->signalDepth   = depth;

        return ok;
    });
}

void test_signal_manager(RequestLayerCaps* caps)
{
    const auto  native_coverage = arch_task_context_supported() ? syscall_coverage_t::Tested : syscall_coverage_t::Unsupported;
    const char* native_reason   = arch_task_context_supported() ? nullptr : "signal frame fixture requires x86-64 ABI";

    static const syscall_test_t<SignalRequestManager> tests[] = {
        {"HandleRt_sigreturnRequest", &SignalRequestManager::HandleRt_sigreturnRequest, SYSCALL_rt_sigreturn, validate_signal<SYSCALL_rt_sigreturn>, native_coverage,
            native_reason},
        {"HandleRt_sigprocmaskRequest", &SignalRequestManager::HandleRt_sigprocmaskRequest, SYSCALL_rt_sigprocmask, validate_signal<SYSCALL_rt_sigprocmask>},
        {"HandleRt_sigactionRequest", &SignalRequestManager::HandleRt_sigactionRequest, SYSCALL_rt_sigaction, validate_signal<SYSCALL_rt_sigaction>},
        {"HandleSigaltstackRequest", &SignalRequestManager::HandleSigaltstackRequest, SYSCALL_sigaltstack, validate_signal<SYSCALL_sigaltstack>},
        {"HandleKillRequest", &SignalRequestManager::HandleKillRequest, SYSCALL_kill, validate_signal<SYSCALL_kill>},
        {"HandleTgkillRequest", &SignalRequestManager::HandleTgkillRequest, SYSCALL_tgkill, validate_signal<SYSCALL_tgkill>},
        {"HandlePauseRequest", &SignalRequestManager::HandlePauseRequest, SYSCALL_pause, validate_signal<SYSCALL_pause>},
    };
    SignalRequestManager unavailable(nullptr, nullptr);

    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};

        if ((unavailable.*test.method)(&frame) != LINUX_ENOSYS || (caps->signalRequestManager != nullptr && (caps->signalRequestManager->*test.method)(nullptr) != LINUX_EINVAL))
        {
            expect(false, "signal null-frame/missing-dependency validation");
        }
    }

    run_syscalls(caps->signalRequestManager, tests);
}

} // namespace request_selftest
