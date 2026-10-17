#include "RequestTestSupport.hpp"

namespace request_selftest
{
static bool validate_sched_yield(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t dispatchedFrame = *(ctx->frame);

    dispatchedFrame.syscall_number = SYSCALL_sched_yield;

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&dispatchedFrame);

    return REQUEST_EXPECT(dispatchResult == 0);
}

static bool validate_getcpu(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    uint32_t cpuValue  = UINT32_MAX;
    uint32_t nodeValue = UINT32_MAX;

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_getcpu;
    frame.arg0           = request_user_address(&cpuValue, sizeof(cpuValue));
    frame.arg1           = request_user_address(&nodeValue, sizeof(nodeValue));
    frame.arg2           = 0;

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&frame);

    bool ok = equal(dispatchResult, 0, "getcpu succeeds with valid outputs");

    ok &= equal(cpuValue, arch_cpu_id(), "getcpu reports the executing CPU");

    ok &= equal(nodeValue, platform.cpus[arch_cpu_id()].numa_node - platform.numa_nodes, "getcpu reports the executing CPU NUMA node");

    frame.arg0 = CANONICAL_KERNEL_BASE;

    ok &= equal(dispatcher_dispatch_syscall(&frame), LINUX_EFAULT, "getcpu rejects kernel output pointers");

    return ok;
}

static bool test_scheduling_policies()
{
    cpu_preemption_disable();
    auto dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto resources = dispatcher->GetResourceLayerCaps();
    auto scheduler = dispatcher->GetLogicLayerCaps()->scheduler;
    auto tasks = resources->taskManager;
    task_t* caller = tasks->GetCurrentTask();
    process_t* process = resources->processManager->GetCurrentProcess();
    scheduling_attributes_t previous = {}, attributes = {};
    scheduler->ReadPolicy(caller, &previous);
    const uint64_t affinity = caller->affinityMask;
    const uint8_t cpu = arch_cpu_id();
    task_t* peer = tasks->AllocateTask();
    if (!expect(peer != nullptr && resources->processManager->AddTask(process, peer), "allocate policy selection peer"))
    {
        if (peer != nullptr)
        {
            tasks->FreeTask(peer);
        }
        cpu_preemption_enable();
        return false;
    }
    bool ok = expect(scheduler->SetAffinity(caller, 1ULL << cpu) == 0 && scheduler->SetAffinity(peer, 1ULL << cpu) == 0, "partition policy fixtures on executing CPU");
    attributes.size = sizeof(attributes);
    attributes.policy = SCHEDULING_FIFO;
    attributes.priority = 40;
    ok &= expect(scheduler->ConfigurePolicy(caller, attributes) == 0 && scheduler->ConfigurePolicy(peer, attributes) == 0, "FIFO priority activation");
    auto select = [&](Scheduler::scheduling_reason_t reason)
    {
        caller->schedulerQueued = peer->schedulerQueued = true;
        const auto decision = scheduler->RunSchedulingAlgorithm(cpu, reason);
        task_t* selected = decision.task;
        if (selected != nullptr)
        {
            tasks->ReleaseDispatchClaim(selected);
        }
        return selected;
    };
    ok &= expect(select(Scheduler::scheduling_reason_t::TIMER) == caller, "FIFO equal-priority timer preserves head");
    ok &= expect(select(Scheduler::scheduling_reason_t::YIELD) == peer, "FIFO yield advances to equal-priority peer");
    int32_t publicPriority = 40;
    const uint64_t parameterAddress = request_user_address(&publicPriority, sizeof(publicPriority));
    ok &= expect(syscall_call(SYSCALL_sched_setscheduler, 0, SCHEDULING_RR, parameterAddress) == 0 && syscall_call(SYSCALL_sched_getscheduler, 0) == SCHEDULING_RR, "public policy request installs and reports RR");
    ok &= expect(syscall_call(SYSCALL_sched_get_priority_min, SCHEDULING_RR) == 1 && syscall_call(SYSCALL_sched_get_priority_max, SCHEDULING_FIFO) == 99, "public RT priority range");
    time_timespec_t interval = {};
    const uint64_t intervalAddress = request_user_address(&interval, sizeof(interval));
    ok &= expect(syscall_call(SYSCALL_sched_rr_get_interval, 0, intervalAddress) == 0 && interval.seconds == 0 && interval.nanoseconds == static_cast<int64_t>(SCHEDULING_RR_QUANTUM), "public RR quantum ABI");
    attributes.policy = SCHEDULING_RR;
    ok &= expect(scheduler->ConfigurePolicy(caller, attributes) == 0 && scheduler->ConfigurePolicy(peer, attributes) == 0, "RR policy activation");
    scheduler->AccountSchedulingCurrent(true);
    uint64_t start = 0, now = 0;
    arch_time_read_monotonic(&start);
    do
    {
        arch_pause();
        arch_time_read_monotonic(&now);
    } while (now - start < 1000000);
    scheduler->AccountSchedulingCurrent(false);
    const uint64_t remaining = caller->scheduling->roundRobinRemaining;
    ok &= expect(remaining < SCHEDULING_RR_QUANTUM && remaining > 0, "RR charges actual elapsed execution");
    ok &= expect(select(Scheduler::scheduling_reason_t::RESCHEDULE) == caller && caller->scheduling->roundRobinRemaining <= remaining, "RR preserves unexpired quantum across reschedule");
    caller->scheduling->roundRobinRemaining = 0;
    ok &= expect(select(Scheduler::scheduling_reason_t::TIMER) == peer, "RR quantum expiry rotates equal-priority tasks");
    attributes = {sizeof(attributes), SCHEDULING_DEADLINE, 0, 0, 0, 8000000, 10000000, 10000000, 0, 0};
    ok &= expect(scheduler->ConfigurePolicy(caller, attributes) == 0, "deadline accepts 80 percent partition reservation");
    attributes.runtime = 1000000;
    ok &= expect(scheduler->ConfigurePolicy(peer, attributes) == static_cast<uint64_t>(-16), "deadline rejects overcommitted partition atomically");
    scheduling_attributes_t actual = {};
    scheduler->ReadPolicy(peer, &actual);
    ok &= expect(actual.policy == SCHEDULING_RR, "failed admission preserves old policy");
    ok &= expect(scheduler->ValidateClonePolicy(caller) == LINUX_EAGAIN, "deadline fork cannot duplicate reservation");
    attributes.flags = 1;
    ok &= expect(scheduler->ConfigurePolicy(caller, attributes) == 0 && scheduler->ValidateClonePolicy(caller) == 0, "reset-on-fork allows deadline parent");
    task_t* child = tasks->AllocateTask();
    if (child != nullptr)
    {
        scheduler->InheritPolicy(caller, child);
        ok &= expect(child->scheduling->policy == SCHEDULING_OTHER && child->scheduling->reservationCpu == -1, "reset child receives no reservation or PI donation");
        ok &= cleanup(tasks->FreeTask(child), "free policy inheritance child");
    }
    // Measure selection from a fresh interval: inheritance fixture setup is
    // not part of the deadline execution interval exercised below.
    scheduler->AccountSchedulingCurrent(true);
    ok &= expect(select(Scheduler::scheduling_reason_t::TIMER) == caller, "deadline class precedes real-time class");
    scheduler->AccountSchedulingCurrent(true);
    arch_time_read_monotonic(&start);
    do
    {
        arch_pause();
        arch_time_read_monotonic(&now);
    } while (now - start < 2000000);
    scheduler->AccountSchedulingCurrent(false);
    ok &= expect(caller->scheduling->deadlineRemaining == 0 && select(Scheduler::scheduling_reason_t::TIMER) == peer, "exhausted deadline reservation yields to eligible real-time peer");
    const uint64_t release = caller->scheduling->deadlineRelease;
    do
    {
        arch_pause();
        arch_time_read_monotonic(&now);
    } while (now < release);
    // No context switched in this fixture: begin a new accounting interval so
    // elapsed test bookkeeping is not charged as a resumed reservation.
    scheduler->AccountSchedulingCurrent(true);
    ok &= expect(select(Scheduler::scheduling_reason_t::TIMER) == caller && caller->scheduling->deadlineRemaining != 0, "deadline budget replenishes at next period");
    const auto credentials = process->credentials.user;
    process->credentials.user.effective = 1000;
    attributes.policy = SCHEDULING_FIFO;
    attributes.priority = 60;
    attributes.runtime = attributes.deadline = attributes.period = 0;
    ok &= expect(scheduler->ConfigurePolicy(peer, attributes) == static_cast<uint64_t>(-1), "unprivileged real-time activation rejected");
    process->credentials.user = credentials;
    attributes.policy = 3;
    ok &= expect(scheduler->ConfigurePolicy(peer, attributes) == LINUX_EOPNOTSUPP, "unenforced policy explicitly rejected");
    attributes = {};
    attributes.size = sizeof(attributes);
    scheduler->ConfigurePolicy(peer, attributes);
    peer->schedulerQueued = false;
    resources->processManager->RemoveTask(process, peer);
    ok &= cleanup(tasks->FreeTask(peer), "free policy selection peer");
    ok &= expect(scheduler->ConfigurePolicy(caller, previous) == 0 && scheduler->SetAffinity(caller, affinity) == 0, "restore caller policy and partition");
    cpu_preemption_enable();
    return ok;
}

static bool validate_sched_getaffinity(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    uint8_t mask[sizeof(uint64_t)] = {};

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_sched_getaffinity;
    frame.arg0           = 0;
    frame.arg1           = sizeof(mask);
    frame.arg2           = request_user_address(mask, sizeof(mask));

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&frame);

    if (dispatchResult != sizeof(uint64_t))
    {
        return false;
    }

    uint64_t affinityMask = 0;

    memcpy(&affinityMask, mask, sizeof(affinityMask));

    const uint64_t cpuCount     = platform.cpu_count < BOOT_SMP_MAX_CPUS ? platform.cpu_count : BOOT_SMP_MAX_CPUS;
    const uint64_t expectedMask = cpuCount == 64 ? UINT64_MAX : (1ULL << cpuCount) - 1;

    bool ok = equal(affinityMask, expectedMask, "affinity contains the boot CPU set");

    const uint64_t restricted = 1ULL << arch_cpu_id();
    uint64_t       inputMask  = restricted;
    const uint64_t input      = request_user_address(&inputMask, sizeof(inputMask));
    ok &= equal(syscall_call(SYSCALL_sched_setaffinity, 0, 8, input), 0, "affinity restriction is stored");
    ok &= equal(dispatcher_dispatch_syscall(&frame), 8, "restricted affinity query");
    memcpy(&affinityMask, mask, sizeof(affinityMask));
    ok &= equal(affinityMask, restricted, "affinity query returns stored restriction");
    inputMask = 0;
    ok &= equal(syscall_call(SYSCALL_sched_setaffinity, 0, 8, input), LINUX_EINVAL, "empty affinity rejected");
    if (platform.cpu_count > 1)
    {
        const uint32_t originalCpu = arch_cpu_id();
        const uint32_t destination = (originalCpu + 1) % platform.cpu_count;
        inputMask                  = 1ULL << destination;
        ok &= equal(syscall_call(SYSCALL_sched_setaffinity, 0, 8, input), 0, "self affinity migration completes");
        ok &= equal(arch_cpu_id(), destination, "task actually executes on restricted destination");
        inputMask = restricted;
        ok &= equal(syscall_call(SYSCALL_sched_setaffinity, 0, 8, input), 0, "self migration returns to original CPU");
        ok &= equal(arch_cpu_id(), originalCpu, "original CPU restored after migration");
    }
    inputMask = expectedMask;
    ok &= equal(syscall_call(SYSCALL_sched_setaffinity, 0, 8, input), 0, "restore affinity after restriction");
    ok &= test_scheduling_policies();
    const uint64_t previousPriority = syscall_call(SYSCALL_getpriority, 0, 0);
    ok &= equal(syscall_call(SYSCALL_setpriority, 0, 0, 10), 0, "ordinary task niceness changes");
    ok &= equal(syscall_call(SYSCALL_getpriority, 0, 0), 10, "getpriority uses raw syscall encoding");
    ok &= equal(syscall_call(SYSCALL_setpriority, 0, 0, 20 - previousPriority), 0, "restore task niceness");
    ok &= equal(syscall_call(SYSCALL_getpriority, 1, 0), previousPriority, "group selector returns highest member priority");
    ok &= equal(syscall_call(SYSCALL_getpriority, 2, 0), previousPriority, "user selector returns highest member priority");
    ok &= equal(syscall_call(SYSCALL_setpriority, 1, UINT32_MAX, 0), LINUX_ESRCH, "missing priority group rejected");

    arch_syscall_frame_t shortFrame = frame;

    shortFrame.arg1 = sizeof(uint32_t);

    const uint64_t shortResult = dispatcher_dispatch_syscall(&shortFrame);

    ok &= equal(shortResult, LINUX_EINVAL, "affinity rejects undersized CPU sets");

    frame.arg2 = CANONICAL_KERNEL_BASE;

    ok &= equal(dispatcher_dispatch_syscall(&frame), LINUX_EFAULT, "affinity rejects kernel output pointers");

    return ok;
}

void test_scheduler_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<SchedulerRequestManager> tests[] = {
        {"HandleSched_yieldRequest", &SchedulerRequestManager::HandleSched_yieldRequest, SYSCALL_sched_yield, validate_sched_yield},
        {"HandleSched_getaffinityRequest", &SchedulerRequestManager::HandleSched_getaffinityRequest, SYSCALL_sched_getaffinity, validate_sched_getaffinity},
        {"HandleGetcpuRequest", &SchedulerRequestManager::HandleGetcpuRequest, SYSCALL_getcpu, validate_getcpu},
    };

    run_syscalls(caps->schedulerRequestManager, tests);
}

} // namespace request_selftest
