#include "RequestTestSupport.hpp"

namespace request_selftest
{
static bool test_time_clock(process_t* process, uint64_t number)
{
    uint64_t base = fixture_map(2);

    if (static_cast<int64_t>(base) < 0)
    {
        return expect(false, "time fixture mapping");
    }

    uint64_t     output                          = base + PAGE_SIZE - 8;
    const size_t outputSize                      = number == SYSCALL_time ? 8 : 16;
    *fixture_bytes(process, output - 1)          = 0xa5;
    *fixture_bytes(process, output + outputSize) = 0xa5;
    time_timespec_t value                        = {};

    uint64_t result = number == SYSCALL_time           ? syscall_call(number, output)
                      : number == SYSCALL_gettimeofday ? syscall_call(number, output, base + 64)
                                                       : syscall_call(number, 1, output);

    bool ok = expect(number == SYSCALL_time ? static_cast<int64_t>(result) >= 0 : result == 0, "time dispatcher success");

    ok &= expect(*fixture_bytes(process, output - 1) == 0xa5 && *fixture_bytes(process, output + outputSize) == 0xa5, "time writes exact ABI size");

    fixture_load(process, output, &value, number == SYSCALL_time ? 8 : 16);

    if (number == SYSCALL_time)
    {
        ok &= expect(result == static_cast<uint64_t>(value.seconds), "time return matches optional seconds output");

        ok &= expect(syscall_call(number) >= result, "time null output allowed");
    }
    else
    {
        ok &= expect(value.seconds >= 0 && value.nanoseconds >= 0 && value.nanoseconds < (number == SYSCALL_gettimeofday ? 1000000 : 1000000000), "normalized time ABI");

        if (number == SYSCALL_gettimeofday)
        {
            int32_t timezone[2] = {-1, -1};

            fixture_load(process, base + 64, timezone, sizeof(timezone));

            ok &= expect(timezone[0] == 0 && timezone[1] == 0 && syscall_call(number) == 0, "gettimeofday UTC timezone and optional outputs");

            ok &= expect(syscall_call(number, 0, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "gettimeofday invalid timezone output");
        }
        else
        {
            ok &= expect(syscall_call(number, 2, output) == LINUX_EINVAL && syscall_call(number, UINT64_MAX, output) == LINUX_EINVAL, "unsupported clock IDs");

            ok &= expect(syscall_call(number, 1, 0) == (number == SYSCALL_clock_getres ? 0 : LINUX_EFAULT), "clock nullable output semantics");

            if (number == SYSCALL_clock_getres)
            {
                ok &= expect(value.seconds > 0 || value.nanoseconds > 0, "clock positive resolution");
            }
            else
            {
                time_timespec_t next;

                ok &= expect(syscall_call(number, 1, output) == 0, "repeat monotonic clock read");

                fixture_load(process, output, &next, sizeof(next));

                ok &= expect(next.seconds > value.seconds || (next.seconds == value.seconds && next.nanoseconds >= value.nanoseconds), "monotonic clock never reverses");
            }
        }
    }

    auto call_output = [number](uint64_t pointer)
    {
        return number == SYSCALL_time || number == SYSCALL_gettimeofday ? syscall_call(number, pointer) : syscall_call(number, 1, pointer);
    };
    ok &= expect(call_output(CANONICAL_KERNEL_BASE) == LINUX_EFAULT && call_output(0x400000) == LINUX_EFAULT, "time rejects kernel and unmapped output");
    *fixture_bytes(process, base + 2 * PAGE_SIZE - 4) = 0xa5;
    ok &=
        expect(call_output(base + 2 * PAGE_SIZE - 4) == LINUX_EFAULT && *fixture_bytes(process, base + 2 * PAGE_SIZE - 4) == 0xa5, "time preflights entire output before writing");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);

    ok &= expect(call_output(base) == LINUX_EFAULT, "time readonly output rejected");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 0);

    ok &= expect(call_output(base) == LINUX_EFAULT, "time PROT_NONE output rejected");

    return ok;
}

struct sleep_peer_context_t
{
    Scheduler*      scheduler;
    ProcessManager* processManager;
    TaskManager*    taskManager;
    process_t*      sleeper;
    task_t*         sleepingTask;
    bool            entered;
    bool            protectedLifetime;
    bool            blockedDispatch;
};

static void sleep_peer_entry(void* argument)
{
    // Observe the parked sleeper before a timer wake can change its state.
    cpu_preemption_disable();

    sleep_peer_context_t* context = static_cast<sleep_peer_context_t*>(argument);

    context->entered           = true;
    context->protectedLifetime = !context->taskManager->FreeTask(context->sleepingTask) && !context->processManager->FreeProcess(context->sleeper);
    context->blockedDispatch   = !context->taskManager->ExecuteTask(context->sleepingTask);

    cpu_preemption_enable();

    process_t* current = context->processManager->GetCurrentProcess();

    // Leave no ready entry referring to this deliberately suspended test peer.
    context->scheduler->KillProcess(current->id, 0);

    for (;;)
    {
        // Only the hardware timer may mark the sleeper ready in this peer.
        context->scheduler->RunNextReadyProcess(arch_cpu_id());

        arch_halt();
    }
}

static bool test_time_sleep(process_t* process, uint64_t number)
{
    uint64_t base = fixture_map(2);

    if (static_cast<int64_t>(base) < 0)
    {
        return expect(false, "sleep fixture mapping");
    }

    uint64_t input = base + PAGE_SIZE - 8;
    auto     call  = [number](uint64_t pointer, uint64_t clock = 1, uint64_t flags = 0)
    {
        return number == SYSCALL_nanosleep ? syscall_call(number, pointer, CANONICAL_KERNEL_BASE) : syscall_call(number, clock, flags, pointer, CANONICAL_KERNEL_BASE);
    };
    bool ok = expect(call(0) == LINUX_EFAULT && call(CANONICAL_KERNEL_BASE) == LINUX_EFAULT && call(base + 2 * PAGE_SIZE - 8) == LINUX_EFAULT, "sleep invalid request buffers");

    time_timespec_t request = {-1, 0};

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(call(input) == LINUX_EINVAL, "sleep negative seconds");

    request = {0, 1000000000};

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(call(input) == LINUX_EINVAL, "sleep invalid nanoseconds");

    Dispatcher*        dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps* resources  = dispatcher->GetResourceLayerCaps();
    Scheduler*         scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    time_timespec_t    clockProbe;

    if (resources->timeManager == nullptr || resources->timeManager->ReadClock(1, &clockProbe) != 0)
    {
        // Retain failure reporting without creating a runnable peer that a
        // missing clock cannot park/wake the current task to exercise.
        return expect(false, "sleep requires monotonic clock; inspect boot clock diagnostic");
    }

    sleep_peer_context_t peerContext = {scheduler, resources->processManager, resources->taskManager, process, resources->taskManager->GetCurrentTask(), false, false, false};
    virt_addr_space_t*   peerSpace   = resources->virtualMemoryManager->CreateUserAddressSpace();
    process_t*           peerProcess = peerSpace != nullptr ? resources->processManager->CreateProcess(peerSpace) : nullptr;
    task_t*              peerTask    = resources->taskManager->CreateKernelTask(sleep_peer_entry, &peerContext);

    // Publish the peer only when the fixture can enter sleep atomically with
    // respect to timer dispatch. ExecuteTask transfers this guard to the sleeper.
    cpu_preemption_disable();

    bool peerReady =
        peerProcess != nullptr && peerTask != nullptr && resources->processManager->AddTask(peerProcess, peerTask) && scheduler->EnqueueProcess(arch_cpu_id(), peerProcess->id);

    ok &= expect(peerReady, "sleep runnable peer fixture");

    if (!peerReady)
    {
        cpu_preemption_enable();

        if (peerTask != nullptr)
        {
            resources->taskManager->FreeTask(peerTask);
        }

        if (peerProcess != nullptr)
        {
            resources->processManager->FreeProcess(peerProcess);
        }

        if (peerSpace != nullptr)
        {
            resources->virtualMemoryManager->DestroyUserAddressSpace(peerSpace);
        }

        return false;
    }

    request = {0, 100000000};

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(syscall_call(SYSCALL_clock_gettime, 1, base + 64) == 0, "sleep monotonic start");

    time_timespec_t before = {}, after = {};

    fixture_load(process, base + 64, &before, sizeof(before));

    ok &= expect(call(input) == 0, "scheduler sleep and ignore remainder on success");

    cpu_preemption_enable();

    ok &= expect(peerContext.entered && peerContext.protectedLifetime && peerContext.blockedDispatch, "sleep lets a peer run while protecting and blocking the sleeper");

    // If the clock failed before switching, remove the still-queued peer too.

    if (!peerContext.entered)
    {
        scheduler->KillProcess(peerProcess->id, 0);
    }

    bool releasedTask    = !peerTask->allocated || resources->taskManager->FreeTask(peerTask);
    bool releasedProcess = !peerProcess->allocated || resources->processManager->FreeProcess(peerProcess);

    ok &= cleanup(releasedTask && releasedProcess, "sleep peer cleanup");

    resources->virtualMemoryManager->DestroyUserAddressSpace(peerSpace);

    ok &= expect(syscall_call(SYSCALL_clock_gettime, 1, base + 64) == 0, "sleep monotonic end");

    fixture_load(process, base + 64, &after, sizeof(after));

    int64_t elapsed = (after.seconds - before.seconds) * 1000000000 + after.nanoseconds - before.nanoseconds;

    ok &= expect(elapsed >= 100000000, "sleep waits at least requested duration");

    request = {INT64_MAX, 0};

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(call(input) == static_cast<uint64_t>(-75), "sleep rejects unrepresentable duration");

    request = {0, 0};

    fixture_store(process, input, &request, sizeof(request));

    if (number == SYSCALL_clock_nanosleep)
    {
        ok &= expect(call(input, 1, 1) == 0 && call(input, 0, 1) == 0, "absolute past deadlines return immediately");

        ok &= expect(call(input, 2) == LINUX_EINVAL && call(input, 1, 2) == LINUX_EINVAL, "sleep unsupported clock and flags");

        request = after;
        request.nanoseconds += 1000000;

        if (request.nanoseconds >= 1000000000)
        {
            ++request.seconds;
            request.nanoseconds -= 1000000000;
        }

        fixture_store(process, input, &request, sizeof(request));

        ok &= expect(call(input, 1, 1) == 0, "absolute monotonic sleep");

        syscall_call(SYSCALL_clock_gettime, 1, base + 64);

        fixture_load(process, base + 64, &after, sizeof(after));

        ok &= expect(after.seconds > request.seconds || (after.seconds == request.seconds && after.nanoseconds >= request.nanoseconds), "absolute sleep reaches deadline");
    }

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);

    request = {0, 0};

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(call(input) == 0, "sleep readonly input accepted");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 0);

    ok &= expect(call(input) == LINUX_EFAULT, "sleep PROT_NONE input rejected");

    return ok;
}

static bool test_time_real_expiration(process_t* process, uint64_t base)
{
    Dispatcher*        dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps* resources  = dispatcher->GetResourceLayerCaps();
    TimeManager*       clock      = resources->timeManager;
    SignalManager*     signals    = dispatcher->GetLogicLayerCaps()->signalManager;
    task_t*            task       = resources->taskManager->GetCurrentTask();
    time_timespec_t    now        = {};

    if (clock == nullptr || clock->ReadClock(1, &now) != 0)
    {
        return expect(false, "real timer expiration requires a working monotonic clock");
    }

    const uint64_t  alarmBit = 1ULL << 13;
    const uint64_t  oldMask  = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);
    signal_action_t previous = {};
    signal_action_t action   = {base + 512, 0x04000004, base + 768, 0};

    bool ok = expect(signals->Action(14, &action, &previous) == 0, "real timer signal handler fixture");

    __atomic_store_n(&task->signalMask, oldMask | alarmBit, __ATOMIC_RELEASE);

    time_itimerval_t request = {{0, 0}, {10, 0}};

    fixture_store(process, base + 256, &request, sizeof(request));

    const bool armed = syscall_call(SYSCALL_setitimer, 0, base + 256) == 0;

    ok &= expect(armed, "single-shot real timer arming");

    if (armed)
    {
        spinlock_acquire_preempt(&process->timerLock);

        const uint64_t deadline = process->realTimerDeadline;

        spinlock_release_preempt(&process->timerLock);

        ok &= expect(!clock->ExpireRealTimer(process, deadline - 1) && clock->ExpireRealTimer(process, deadline) && !clock->ExpireRealTimer(process, deadline),
            "real timer expires once at deadline");

        ok &= expect((__atomic_load_n(&process->signalPending, __ATOMIC_ACQUIRE) & alarmBit) != 0 && process->signalSenders[13].code == 128, "real timer queues kernel SIGALRM");

        syscall_call(SYSCALL_getitimer, 0, base + 320);

        time_itimerval_t actual = {};

        fixture_load(process, base + 320, &actual, sizeof(actual));

        ok &= expect(actual.value.seconds == 0 && actual.value.microseconds == 0, "expired single-shot timer is disarmed");

        ok &= expect(syscall_call(SYSCALL_alarm, 0) == 0 && (__atomic_load_n(&process->signalPending, __ATOMIC_ACQUIRE) & alarmBit) != 0,
            "timer cancellation preserves already pending SIGALRM");

        __atomic_fetch_and(&process->signalPending, ~alarmBit, __ATOMIC_RELEASE);
    }

    request = {{1, 0}, {10, 0}};

    fixture_store(process, base + 256, &request, sizeof(request));

    const bool periodic = syscall_call(SYSCALL_setitimer, 0, base + 256) == 0;

    ok &= expect(periodic, "periodic real timer arming");

    if (periodic)
    {
        // Exercise the retained timer at its exact deadline. The scheduler
        // uses the same expiration method with the current monotonic sample.
        spinlock_acquire_preempt(&process->timerLock);

        const uint64_t deadline = process->realTimerDeadline;

        spinlock_release_preempt(&process->timerLock);

        ok &= expect(clock->ExpireRealTimer(process, deadline) && !process->realTimerArmed && process->realTimerExpired, "periodic timer waits for pending signal consumption");

#if defined(__x86_64__)
        alignas(16) uint8_t floating[512] = {};

        const uint32_t mxcsr     = 0x1f80;
        const uint32_t mxcsrMask = 0xffbf;

        memcpy(floating + 24, &mxcsr, sizeof(mxcsr));

        memcpy(floating + 28, &mxcsrMask, sizeof(mxcsrMask));

        arch_syscall_frame_t frame = {};

        frame.syscall_number = SYSCALL_getitimer;
        frame.user_cs        = USER_CS;
        frame.user_ss        = USER_SS;
        frame.user_rflags    = 0x202;
        frame.user_rip       = base + 1024;
        frame.user_rsp       = base + 2 * PAGE_SIZE - 16;
        frame.fxstate        = (uint64_t) floating;

        __atomic_store_n(&task->signalMask, oldMask & ~alarmBit, __ATOMIC_RELEASE);

        ok &= expect(signals->Deliver(&frame, 0) == 0 && frame.arg0 == 14 && process->realTimerArmed && !process->realTimerExpired, "SIGALRM delivery rearms periodic timer");

        frame.user_rsp += 8;

        ok &= expect(signals->Restore(&frame) == 0, "real timer signal frame restores context");
#else
        clock->RestartRealTimer(process);

        ok &= expect(process->realTimerArmed, "periodic timer rearm");
#endif

        syscall_call(SYSCALL_setitimer, 0, 0);

        __atomic_fetch_and(&process->signalPending, ~alarmBit, __ATOMIC_RELEASE);
    }

    request = {{0, 0}, {0, 10000}};

    fixture_store(process, base + 256, &request, sizeof(request));

    __atomic_store_n(&task->signalMask, oldMask & ~alarmBit, __ATOMIC_RELEASE);

    const bool waiting = syscall_call(SYSCALL_setitimer, 0, base + 256) == 0;

    ok &= expect(waiting, "parked real timer fixture");

    if (waiting)
    {
        ok &= expect(syscall_call(SYSCALL_poll, 0, 0, UINT64_MAX) == (uint64_t) -4, "real timer tick interrupts parked event wait with SIGALRM");
    }

    syscall_call(SYSCALL_setitimer, 0, 0);

    __atomic_fetch_and(&process->signalPending, ~alarmBit, __ATOMIC_RELEASE);

    __atomic_store_n(&task->signalMask, oldMask, __ATOMIC_RELEASE);

    (void) signals->Action(14, &previous, nullptr);

    return ok;
}

static bool test_time_timer(process_t* process, uint64_t number)
{
    uint64_t base = fixture_map(2);

    if (static_cast<int64_t>(base) < 0)
    {
        return expect(false, "timer fixture mapping");
    }

    uint64_t         input   = base + PAGE_SIZE - 8;
    uint64_t         output  = base + 64;
    bool             ok      = true;
    time_itimerval_t request = {{2, 123}, {0, 0}}, value = {};

    for (unsigned which = 0; which < 3; ++which)
    {
        ok &= expect(syscall_call(SYSCALL_getitimer, which, output) == 0, "new process timer query");

        fixture_load(process, output, &value, sizeof(value));

        ok &= expect(value.interval.seconds == 0 && value.interval.microseconds == 0 && value.value.seconds == 0 && value.value.microseconds == 0, "new process timers disarmed");
    }

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(syscall_call(SYSCALL_setitimer, 0, input, output) == 0 && syscall_call(SYSCALL_getitimer, 0, output) == 0, "disarmed real timer clears interval");

    fixture_load(process, output, &value, sizeof(value));

    ok &= expect(value.interval.seconds == 0 && value.interval.microseconds == 0 && value.value.seconds == 0, "disarmed real timer interval roundtrip");

    TimeManager*    clock     = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->timeManager;
    time_timespec_t now       = {};
    const bool      available = clock != nullptr && clock->ReadClock(1, &now) == 0;

    if (!expect(available, "implemented timers require a working monotonic clock"))
    {
        return false;
    }

    ok &= test_time_real_expiration(process, base);

    if (number == SYSCALL_alarm)
    {
        if (available)
        {
            ok &= expect(syscall_call(number, 2) == 0 && syscall_call(number, 1) == 2 && syscall_call(number, 0) == 1 && syscall_call(number, 0) == 0,
                "alarm arm, replacement and cancellation return remaining seconds");

            ok &= expect(syscall_call(number, (1ULL << 32) | 2) == 0 && syscall_call(number, 0) == 2, "alarm uses unsigned 32-bit seconds");
        }
        else
        {
            ok &= expect(syscall_call(number, 1) == LINUX_ENOSYS && syscall_call(number, 0) == 0, "alarm reports unavailable clock and permits disarmed cancellation");
        }

        syscall_call(SYSCALL_getitimer, 0, output);

        fixture_load(process, output, &value, sizeof(value));

        ok &= expect(value.interval.seconds == 0 && value.interval.microseconds == 0, "alarm cancellation clears real timer interval");

        return ok;
    }

    ok &= expect(syscall_call(SYSCALL_getitimer, 3, output) == LINUX_EINVAL && syscall_call(SYSCALL_setitimer, UINT64_MAX, input) == LINUX_EINVAL, "timer selector validation");

    ok &= expect(syscall_call(SYSCALL_getitimer, 0, CANONICAL_KERNEL_BASE) == LINUX_EFAULT && syscall_call(SYSCALL_setitimer, 0, CANONICAL_KERNEL_BASE) == LINUX_EFAULT,
        "timer kernel buffers rejected");

    ok &= expect(syscall_call(SYSCALL_setitimer, 0, input, base + 2 * PAGE_SIZE - 4) == LINUX_EFAULT, "timer invalid old-value output");

    request.value.seconds = 1;

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(syscall_call(SYSCALL_setitimer, 0, input) == (available ? 0 : LINUX_ENOSYS), "real timer arming uses monotonic clock");

    syscall_call(SYSCALL_getitimer, 0, output);

    fixture_load(process, output, &value, sizeof(value));

    if (available)
    {
        ok &= expect(value.interval.seconds == 2 && value.interval.microseconds == 123 && value.value.seconds <= 1 && (value.value.seconds != 0 || value.value.microseconds != 0),
            "armed real timer reports remaining duration");

        for (unsigned which = 1; which <= 2; ++which)
        {
            ok &= expect(syscall_call(SYSCALL_setitimer, which, input) == 0, "CPU timer arms against process accounting");

            ok &= expect(syscall_call(SYSCALL_getitimer, which, output) == 0, "CPU timer query succeeds");

            fixture_load(process, output, &value, sizeof(value));

            ok &= expect(value.value.seconds <= 1 && (value.value.seconds != 0 || value.value.microseconds != 0) && value.interval.seconds == 2 && value.interval.microseconds == 123,
                "CPU timer reports remaining duration and interval");

            ok &= expect(syscall_call(SYSCALL_setitimer, which, 0) == 0 && syscall_call(SYSCALL_getitimer, which, output) == 0, "CPU timer cancellation and query");

            fixture_load(process, output, &value, sizeof(value));

            ok &= expect(value.value.seconds == 0 && value.value.microseconds == 0 && value.interval.seconds == 0 && value.interval.microseconds == 0, "cancelled CPU timer is disarmed");
        }

        syscall_call(SYSCALL_setitimer, 0, 0);

        request = {{7, 99}, {5, 0}};

        fixture_store(process, input, &request, sizeof(request));

        ok &= expect(syscall_call(SYSCALL_setitimer, 0, input, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "timer bad old-value output reports EFAULT");

        syscall_call(SYSCALL_getitimer, 0, output);

        fixture_load(process, output, &value, sizeof(value));

        ok &= expect(value.interval.seconds == 7 && value.interval.microseconds == 99 && (value.value.seconds != 0 || value.value.microseconds != 0),
            "timer output fault preserves committed new timer");

        syscall_call(SYSCALL_setitimer, 0, 0);
    }

    request = {{0, 0}, {INT64_MAX, 0}};

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(syscall_call(SYSCALL_setitimer, 0, input) == LINUX_EOVERFLOW, "real timer rejects unrepresentable duration");

    request = {{0, 1000000}, {0, 0}};

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(syscall_call(SYSCALL_setitimer, 0, input) == LINUX_EINVAL, "timer microsecond validation");

    request = {{7, 99}, {0, 0}};

    fixture_store(process, input, &request, sizeof(request));

    ok &= expect(syscall_call(SYSCALL_setitimer, 0, input, input) == 0, "timer aliased input and previous output");

    fixture_load(process, input, &value, sizeof(value));

    ok &= expect(value.interval.seconds == 0 && value.interval.microseconds == 0, "timer alias receives old disarmed interval");

    ok &= expect(syscall_call(SYSCALL_setitimer, 0, 0, output) == 0, "Linux null new-value cancels timer");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_getitimer, 0, output) == LINUX_EFAULT, "timer readonly output rejected");

    return ok;
}

template <uint64_t Number>
static bool validate_time(const syscall_case_t*)
{
    return with_process([](process_t* process)
    {
        if (Number == SYSCALL_nanosleep || Number == SYSCALL_clock_nanosleep)
        {
            return test_time_sleep(process, Number);
        }

        if (Number == SYSCALL_getitimer || Number == SYSCALL_setitimer || Number == SYSCALL_alarm)
        {
            return test_time_timer(process, Number);
        }

        return test_time_clock(process, Number);
    });
}

void test_time_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<TimeRequestManager> tests[] = {
        {"HandleTimeRequest", &TimeRequestManager::HandleTimeRequest, SYSCALL_time, validate_time<SYSCALL_time>},
        {"HandleGettimeofdayRequest", &TimeRequestManager::HandleGettimeofdayRequest, SYSCALL_gettimeofday, validate_time<SYSCALL_gettimeofday>},
        {"HandleClock_gettimeRequest", &TimeRequestManager::HandleClock_gettimeRequest, SYSCALL_clock_gettime, validate_time<SYSCALL_clock_gettime>},
        {"HandleClock_getresRequest", &TimeRequestManager::HandleClock_getresRequest, SYSCALL_clock_getres, validate_time<SYSCALL_clock_getres>},
        {"HandleClock_nanosleepRequest", &TimeRequestManager::HandleClock_nanosleepRequest, SYSCALL_clock_nanosleep, validate_time<SYSCALL_clock_nanosleep>},
        {"HandleNanosleepRequest", &TimeRequestManager::HandleNanosleepRequest, SYSCALL_nanosleep, validate_time<SYSCALL_nanosleep>},
        {"HandleGetitimerRequest", &TimeRequestManager::HandleGetitimerRequest, SYSCALL_getitimer, validate_time<SYSCALL_getitimer>},
        {"HandleSetitimerRequest", &TimeRequestManager::HandleSetitimerRequest, SYSCALL_setitimer, validate_time<SYSCALL_setitimer>},
        {"HandleAlarmRequest", &TimeRequestManager::HandleAlarmRequest, SYSCALL_alarm, validate_time<SYSCALL_alarm>},
    };

    run_syscalls(caps->timeRequestManager, tests);
}

} // namespace request_selftest
