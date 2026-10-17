#include "RequestTestSupport.hpp"

#include <selftests/PollBudget.hpp>

namespace request_selftest
{
struct futex_peer_context_t
{
    Scheduler*         scheduler;
    ResourceLayerCaps* resources;
    task_t*            sleeper;
    uint64_t           address;
    uint32_t           bits;
    bool               selective;
    bool               entered;
    bool               protectedTask;
    uint64_t           wrongAddress;
    uint64_t           wrongBits;
    uint64_t           result;
    process_t*         isolated;
    uint64_t           wrongProcess;
    uint32_t           operationMode;
    uint64_t           mismatch;
    uint64_t           moved;
    uint32_t           compareMode;
};

static void futex_peer_entry(void* argument)
{
    // Publish the peer's observations before yielding to the awakened parent.
    // ExitCurrentTask explicitly switches away with this guard on the dying task.
    cpu_preemption_disable();

    futex_peer_context_t* context = static_cast<futex_peer_context_t*>(argument);

    context->entered       = true;
    context->protectedTask = !context->resources->taskManager->FreeTask(context->sleeper) && !context->resources->taskManager->ExecuteTask(context->sleeper);

    if (context->operationMode == 131 || context->operationMode == 132)
    {
        context->mismatch = context->operationMode == 132 ? syscall_call(SYSCALL_futex, context->address, 132, 0, 1, context->address + 4, 8) : 0;

        context->moved = syscall_call(SYSCALL_futex, context->address, context->operationMode, 0, 1, context->address + 4, 7);

        context->wrongAddress = syscall_call(SYSCALL_futex, context->address, 129, 1);

        context->wrongBits = syscall_call(SYSCALL_futex, context->address + 4, 138, 1, 0, 0, ~context->bits);

        context->result = syscall_call(SYSCALL_futex, context->address + 4, 138, 1, 0, 0, context->bits);

        if (context->result == 0)
        {
            // A failed requeue must not leave this test parked forever.
            syscall_call(SYSCALL_futex, context->address, 129, 1);
        }

        context->scheduler->ExitCurrentTask();
    }

    if (context->operationMode == 133)
    {
        const uint32_t mismatch[] = {8, 7, 7, 6, 7, 8};
        const uint32_t matches[]  = {7, 8, 8, 7, 6, 7};
        const uint32_t compare    = context->compareMode << 24;

        context->mismatch = syscall_call(SYSCALL_futex, context->address + 4, 133, 1, 1, context->address, compare | (7U << 12) | mismatch[context->compareMode]);

        context->result = syscall_call(SYSCALL_futex, context->address + 4, 133, 1, 1, context->address, compare | (11U << 12) | matches[context->compareMode]);

        if (context->result != 1)
        {
            syscall_call(SYSCALL_futex, context->address, 129, 1);
        }

        context->scheduler->ExitCurrentTask();
    }

    context->wrongAddress = syscall_call(SYSCALL_futex, context->address + 4, 129, 1);

    cpu_preemption_disable();

    ProcessManager* processes = context->resources->processManager;
    process_t*      original  = processes->GetCurrentProcess();

    context->wrongProcess = UINT64_MAX;

    if (processes->SetRunningProcess(arch_cpu_id(), context->isolated))
    {
        context->wrongProcess = syscall_call(SYSCALL_futex, context->address, 129, 1);

        if (!processes->SetRunningProcess(arch_cpu_id(), original))
        {
            panic();
        }
    }

    cpu_preemption_enable();

    context->wrongBits = context->selective ? syscall_call(SYSCALL_futex, context->address, 138, 1, 0, 0, ~context->bits) : 0;

    context->result = context->selective ? syscall_call(SYSCALL_futex, context->address, 138, 1, 0, 0, context->bits)
                                         : syscall_call(SYSCALL_futex, context->address, 129, 0); // Legacy nonpositive count wakes one.
    context->scheduler->ExitCurrentTask();
}

static bool test_sync_wake_operation(process_t* process, uint64_t base)
{
    const uint64_t destination = base + 128;

    struct operation_test_t
    {
        uint32_t encoding;
        uint32_t before;
        uint32_t after;
    };

    const operation_test_t tests[] = {
        {0x000ff000U, 123, 255},
        {0x00fff000U, 123, UINT32_MAX},
        {0x10001000U, UINT32_MAX, 0},
        {0x20100000U, 0x00ff, 0x01ff},
        {0x3000f000U, 0x00ff, 0x00f0},
        {0x4000f000U, 0x00ff, 0x00f0},
        {0x80021000U, 123, 2},
    };
    bool ok = true;

    for (const operation_test_t& test : tests)
    {
        fixture_store(process, destination, &test.before, sizeof(test.before));

        ok &= expect(syscall_call(SYSCALL_futex, base, 133, 0, 0, destination, test.encoding) == 0, "futex wake-op updates word without waiters");

        uint32_t actual = 0;

        fixture_load(process, destination, &actual, sizeof(actual));

        ok &= expect(actual == test.after, "futex wake-op arithmetic, sign extension and masked shift");
    }

    uint32_t word = 3;

    fixture_store(process, destination, &word, sizeof(word));

    ok &= expect(syscall_call(SYSCALL_futex, base, 133, 1, 1, destination, 0x50001000U) == LINUX_ENOSYS, "futex wake-op invalid arithmetic operation");

    fixture_load(process, destination, &word, sizeof(word));

    ok &= expect(word == 3, "invalid wake-op arithmetic leaves word untouched");

    ok &= expect(syscall_call(SYSCALL_futex, base, 133, 1, 1, destination, 0x0f009000U) == LINUX_ENOSYS, "futex wake-op invalid comparison");

    fixture_load(process, destination, &word, sizeof(word));

    ok &= expect(word == 9, "invalid wake-op comparison follows atomic update");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_futex, base, 133, 1, 1, destination, 0x00001000U) == LINUX_EFAULT, "futex wake-op requires writable destination");

    fixture_load(process, destination, &word, sizeof(word));

    ok &= expect(word == 9, "failed wake-op output access leaves word untouched");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 3);

    ok &= expect(syscall_call(SYSCALL_futex, base, 133, 1, 1, destination + 1, 0) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_futex, base, 133, 1, 1, base + 2 * PAGE_SIZE, 0) == LINUX_EFAULT,
        "futex wake-op destination alignment and mapping");

    return ok;
}

struct futex_requeue_context_t
{
    ResourceLayerCaps* resources;
    Scheduler*         scheduler;
    task_t*            sourceTask;
    task_t*            destinationTask;
    uint64_t           source;
    uint64_t           destination;
    uint64_t           moved;
    uint64_t           firstWake;
    uint64_t           secondWake;
    uint64_t           destinationResult;
    bool               ordered;
    uint64_t           completion;
};

static void futex_destination_entry(void* argument)
{
    cpu_preemption_disable();

    futex_requeue_context_t* context = static_cast<futex_requeue_context_t*>(argument);

    context->destinationResult = syscall_call(SYSCALL_futex, context->destination, 137, 7, 0, 0, 2);

    __atomic_add_fetch(&context->completion, 1ULL, __ATOMIC_RELEASE);

    context->scheduler->WakeEventWaiters(&context->completion);

    context->scheduler->ExitCurrentTask();
}

static void futex_requeue_entry(void* argument)
{
    cpu_preemption_disable();

    futex_requeue_context_t* context = static_cast<futex_requeue_context_t*>(argument);

    context->moved = syscall_call(SYSCALL_futex, context->source, 132, 0, 1, context->destination, 7);

    context->firstWake = syscall_call(SYSCALL_futex, context->destination, 138, 1, 0, 0, 2);

    context->ordered = !context->destinationTask->sleepBlocked && context->sourceTask->sleepBlocked;

    context->secondWake = syscall_call(SYSCALL_futex, context->destination, 138, 1, 0, 0, 2);

    syscall_call(SYSCALL_futex, context->source, 129, 1);

    context->scheduler->ExitCurrentTask();
}

static bool test_sync_requeue_fifo(process_t* process, uint64_t base)
{
    Dispatcher*             dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps*      resources  = dispatcher->GetResourceLayerCaps();
    Scheduler*              scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    futex_requeue_context_t context    = {resources, scheduler, resources->taskManager->GetCurrentTask(), nullptr, base + 512, base + 516, UINT64_MAX, UINT64_MAX, UINT64_MAX,
        UINT64_MAX, false};
    const uint32_t          words[2]   = {7, 7};

    fixture_store(process, context.source, words, sizeof(words));

    task_t* peers[2] = {
        resources->taskManager->CreateKernelTask(futex_destination_entry, &context),
        resources->taskManager->CreateKernelTask(futex_requeue_entry, &context),
    };
    context.destinationTask = peers[0];

    bool added[2]    = {false, false};
    bool retained[2] = {false, false};

    for (size_t i = 0; i < 2; ++i)
    {
        retained[i] = resources->taskManager->RetainForDispatch(peers[i]);
        added[i]    = retained[i] && resources->processManager->AddTask(process, peers[i]);
    }

    // Start the destination waiter before the requeue peer. A cursor left by a
    // previous fixture can otherwise select the requeue peer first.

    if (added[0] && added[1])
    {
        process->taskCursor = peers[0]->id;
    }

    const bool ready = added[0] && added[1] && scheduler->EnqueueProcess(arch_cpu_id(), process->id);

    bool ok = expect(ready, "futex destination FIFO fixture");

    if (ready)
    {
        const uint64_t result = syscall_call(SYSCALL_futex, context.source, 137, 7, 0, 0, 2);

        ok &= expect(result == 0 && context.moved == 1 && context.firstWake == 1 && context.secondWake == 1 && context.ordered,
            "requeued waiter follows existing destination waiters");

        if (__atomic_load_n(&context.completion, __ATOMIC_ACQUIRE) == 0)
        {
            ok &= expect(scheduler->WaitForEvent(&context.completion, 0, coordination_deadline()) == 0, "futex FIFO destination peer completes before fixture release");
        }

        ok &= expect(context.destinationResult == 0, "existing destination waiter wakes successfully");
    }

    for (size_t i = 0; i < 2; ++i)
    {
        if (added[i])
        {
            if (peers[i]->prev != nullptr)
            {
                peers[i]->prev->next = peers[i]->next;
            }
            else
            {
                process->tasks = peers[i]->next;
            }

            if (peers[i]->next != nullptr)
            {
                peers[i]->next->prev = peers[i]->prev;
            }

            peers[i]->next = peers[i]->prev = nullptr;
        }

        if (peers[i] != nullptr)
        {
            if (retained[i])
            {
                resources->taskManager->ReleaseDispatchClaim(peers[i]);
            }

            ok &= cleanup(resources->taskManager->FreeTask(peers[i]), "futex FIFO peer cleanup");
        }
    }

    return ok;
}

struct pi_test_context_t
{
    Scheduler*         scheduler;
    ResourceLayerCaps* resources;
    uint64_t           first;
    uint64_t           second;
    uint64_t           ready[2];
    uint64_t           release;
    uint64_t           done;
    bool               inherited[2];
    bool               restored[2];
    bool               mediumRan;
    bool               inverted;
    uint64_t           result[2];
};

static void pi_low_owner(void* argument)
{
    auto context = static_cast<pi_test_context_t*>(argument);
    cpu_preemption_disable();
    task_t* task       = context->resources->taskManager->GetCurrentTask();
    context->result[0] = syscall_call(SYSCALL_futex, context->first, 134);
    __atomic_store_n(&context->ready[0], 1ULL, __ATOMIC_RELEASE);
    context->scheduler->WakeEventWaiters(&context->ready[0]);
    if (context->release == 0)
    {
        context->scheduler->WaitForEvent(&context->release, 0, coordination_deadline());
    }
    context->inherited[0] = task->effectiveNice == -10;
    context->inverted |= context->mediumRan;
    context->result[0] |= syscall_call(SYSCALL_futex, context->first, 135);
    context->restored[0] = task->effectiveNice == task->baseNice;
    __atomic_add_fetch(&context->done, 1ULL, __ATOMIC_RELEASE);
    context->scheduler->WakeEventWaiters(&context->done);
    context->scheduler->ExitCurrentTask();
}

static void pi_middle_owner(void* argument)
{
    auto context = static_cast<pi_test_context_t*>(argument);
    cpu_preemption_disable();
    task_t* task       = context->resources->taskManager->GetCurrentTask();
    context->result[1] = syscall_call(SYSCALL_futex, context->second, 134);
    __atomic_store_n(&context->ready[1], 1ULL, __ATOMIC_RELEASE);
    context->scheduler->WakeEventWaiters(&context->ready[1]);
    context->result[1] |= syscall_call(SYSCALL_futex, context->first, 134);
    context->inherited[1] = task->effectiveNice == -10;
    context->result[1] |= syscall_call(SYSCALL_futex, context->first, 135);
    context->result[1] |= syscall_call(SYSCALL_futex, context->second, 135);
    context->restored[1] = task->effectiveNice == task->baseNice;
    __atomic_add_fetch(&context->done, 1ULL, __ATOMIC_RELEASE);
    context->scheduler->WakeEventWaiters(&context->done);
    context->scheduler->ExitCurrentTask();
}

static void pi_medium_competitor(void* argument)
{
    auto context = static_cast<pi_test_context_t*>(argument);
    cpu_preemption_disable();
    context->mediumRan = true;
    __atomic_add_fetch(&context->done, 1ULL, __ATOMIC_RELEASE);
    context->scheduler->WakeEventWaiters(&context->done);
    context->scheduler->ExitCurrentTask();
}

struct pi_cleanup_context_t
{
    Scheduler*         scheduler;
    ResourceLayerCaps* resources;
    uint64_t           address;
    uint64_t           ready;
    uint64_t           release;
    uint64_t           done;
    uint64_t           result;
    bool               die;
    bool               shared;
    bool               inherited;
    uint32_t           observedCpu;
};

static void pi_cleanup_owner(void* argument)
{
    cpu_preemption_disable();
    auto context    = static_cast<pi_cleanup_context_t*>(argument);
    context->result = syscall_call(SYSCALL_futex, context->address, context->shared ? 6 : 134);
    __atomic_store_n(&context->ready, 1ULL, __ATOMIC_RELEASE);
    context->scheduler->WakeEventWaiters(&context->ready);
    if (!context->die)
    {
        context->scheduler->WaitForEvent(&context->release, 0, coordination_deadline());
        context->observedCpu = arch_cpu_id();
        context->inherited   = context->resources->taskManager->GetCurrentTask()->effectiveNice == -10;
        context->result |= syscall_call(SYSCALL_futex, context->address, context->shared ? 7 : 135);
    }
    __atomic_store_n(&context->done, 1ULL, __ATOMIC_RELEASE);
    context->scheduler->WakeEventWaiters(&context->done);
    context->scheduler->ExitCurrentTask();
}

static bool test_pi_cleanup(process_t* process, uint64_t base)
{
    auto          dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto          resources  = dispatcher->GetResourceLayerCaps();
    auto          scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    task_t*       caller     = resources->taskManager->GetCurrentTask();
    const int32_t oldNice    = caller->baseNice;
    scheduler->SetNice(caller, -10);
    bool       ok      = true;
    const bool modes[] = {true, false};
    for (bool death : modes)
    {
        uint32_t word = 0;
        fixture_store(process, base, &word, sizeof(word));
        pi_cleanup_context_t context  = {scheduler, resources, base, 0, 0, 0, UINT64_MAX, death};
        task_t*              peer     = resources->taskManager->CreateKernelTask(pi_cleanup_owner, &context);
        const bool           retained = resources->taskManager->RetainForDispatch(peer);
        const bool           added    = retained && resources->processManager->AddTask(process, peer);
        ok &= expect(added, "PI cleanup owner fixture");
        if (added)
        {
            scheduler->SetNice(peer, 15);
            scheduler->EnqueueProcess(arch_cpu_id(), process->id);
            ok &= expect(scheduler->WaitForEvent(&context.ready, 0, coordination_deadline()) == 0 && context.ready == 1, "PI cleanup owner acquires word");
            if (death)
            {
                fixture_load(process, base, &word, sizeof(word));
                ok &= expect((word & 0x40000000U) != 0 && (word & 0x3fffffffU) == 0, "owner exit publishes OWNER_DIED without a robust-list registration");
                ok &= expect(syscall_call(SYSCALL_futex, base, 134) == 0 && syscall_call(SYSCALL_futex, base, 135) == 0, "PI owner-death recovery acquires and releases word");
            }
            else
            {
                time_timespec_t timeout = {};
                ok &= expect(resources->timeManager->ReadClock(0, &timeout) == 0, "PI timeout clock available");
                timeout.nanoseconds += 2000000;
                if (timeout.nanoseconds >= 1000000000)
                {
                    ++timeout.seconds;
                    timeout.nanoseconds -= 1000000000;
                }
                fixture_store(process, base + 32, &timeout, sizeof(timeout));
                ok &= expect(syscall_call(SYSCALL_futex, base, 134, 0, base + 32) == static_cast<uint64_t>(-110), "PI timeout cancels queued ownership wait");
                ok &= expect(peer->effectiveNice == peer->baseNice, "timeout cancellation removes donation");
                // An already pending signal cancels before parking.
                {
                    SignalDispositionGuard guard(process);
                    SignalManager::Queue(process, caller, 10, {static_cast<int32_t>(process->id), 0, 0});
                }
                ok &= expect(syscall_call(SYSCALL_futex, base, 134) == static_cast<uint64_t>(-4), "PI signal cancellation returns EINTR");
                ok &= expect(peer->effectiveNice == peer->baseNice, "signal cancellation removes donation");
                {
                    SignalDispositionGuard guard(process);
                    SignalManager::Consume(process, caller, 10);
                }
                __atomic_store_n(&context.release, 1ULL, __ATOMIC_RELEASE);
                scheduler->WakeEventWaiters(&context.release);
                ok &= expect(scheduler->WaitForEvent(&context.done, 0, coordination_deadline()) == 0 && context.done == 1, "cancelled PI waiter leaves owner able to unlock");
            }
            ok &= expect(context.result == 0, "PI cleanup owner completed successfully");
            detach_fixture_peer(resources, process, peer);
        }
        if (retained)
        {
            resources->taskManager->ReleaseDispatchClaim(peer);
        }
        if (peer != nullptr)
        {
            ok &= cleanup(resources->taskManager->FreeTask(peer), "PI cleanup owner retirement");
        }
    }
    scheduler->SetNice(caller, oldNice);
    return ok;
}

static bool test_shared_pi(process_t* process)
{
    auto           dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto           resources  = dispatcher->GetResourceLayerCaps();
    auto           scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    const uint64_t address    = fixture_map(1, 0x500000000ULL, 0x21);
    if (static_cast<int64_t>(address) < 0)
    {
        return false;
    }
    uint32_t zero = 0;
    fixture_store(process, address, &zero, sizeof(zero));
    auto           space        = resources->virtualMemoryManager->CreateUserAddressSpace();
    process_t*     ownerProcess = space != nullptr ? resources->processManager->CreateProcess(space) : nullptr;
    const auto     region       = resources->virtualMemoryManager->FindRegion(process->addressSpace, address);
    const uint64_t alias        = address + 32 * PAGE_SIZE;
    bool           mapped       = false;
    if (ownerProcess != nullptr && region != nullptr)
    {
        spinlock_acquire_preempt(&space->operation_lock);
        mapped = resources->virtualMemoryManager->MapBackingPage(space, alias, region->object, region->backing_offset, 3, true, 3);
        spinlock_release_preempt(&space->operation_lock);
    }
    pi_cleanup_context_t context  = {scheduler, resources, alias, 0, 0, 0, UINT64_MAX, false, true, false};
    task_t*              peer     = mapped ? resources->taskManager->CreateKernelTask(pi_cleanup_owner, &context) : nullptr;
    const bool           retained = resources->taskManager->RetainForDispatch(peer);
    const bool           added    = retained && resources->processManager->AddTask(ownerProcess, peer);
    bool                 ok       = expect(added, "shared PI owner has a distinct process and differently addressed shared mapping");
    task_t*              caller   = resources->taskManager->GetCurrentTask();
    const int32_t        oldNice  = caller->baseNice;
    if (added)
    {
        scheduler->SetNice(caller, -10);
        scheduler->SetNice(peer, 15);
        scheduler->EnqueueProcess(arch_cpu_id(), ownerProcess->id);
        ok &= expect(scheduler->WaitForEvent(&context.ready, 0, coordination_deadline()) == 0 && context.ready == 1, "shared PI owner acquires before cross-process waiter");
        const uint32_t destination = platform.cpu_count > 1 ? (arch_cpu_id() + 1) % platform.cpu_count : arch_cpu_id();
        ok &= expect(scheduler->SetAffinity(peer, 1ULL << destination) == 0, "sleeping shared owner accepts restricted destination");
        __atomic_store_n(&context.release, 1ULL, __ATOMIC_RELEASE);
        scheduler->WakeEventWaiters(&context.release);
        ok &= expect(syscall_call(SYSCALL_futex, address, 6) == 0, "shared backing identifies cross-process PI lock");
        ok &= expect(context.inherited && context.result == 0 && context.observedCpu == destination, "shared PI donates across process boundary and preserves migrated sleeper wakeup");
        ok &= expect(syscall_call(SYSCALL_futex, address, 7) == 0, "shared PI handoff releases successfully");
        PollBudget retirement;
        while (__atomic_load_n(&peer->state, __ATOMIC_ACQUIRE) == task_t::RUNNING || __atomic_load_n(&peer->state, __ATOMIC_ACQUIRE) == task_t::SWITCHING)
        {
            if (retirement.Expired())
            {
                SelfTestManager::Get().AbortUnsafe("shared PI owner did not quiesce after remote handoff");
            }
            arch_pause();
        }
        scheduler->SetNice(caller, oldNice);
        detach_fixture_peer(resources, ownerProcess, peer);
    }
    if (retained)
    {
        resources->taskManager->ReleaseDispatchClaim(peer);
    }
    if (peer != nullptr)
    {
        ok &= cleanup(resources->taskManager->FreeTask(peer), "shared PI peer cleanup");
    }
    if (ownerProcess != nullptr)
    {
        ok &= cleanup(resources->processManager->FreeProcess(ownerProcess), "shared PI owner process cleanup");
    }
    if (space != nullptr)
    {
        resources->virtualMemoryManager->DestroyUserAddressSpace(space);
    }
    return ok;
}

static bool test_priority_futex(process_t* process, uint64_t base)
{
    auto          dispatcher   = static_cast<Dispatcher*>(platform.dispacher);
    auto          resources    = dispatcher->GetResourceLayerCaps();
    auto          scheduler    = dispatcher->GetLogicLayerCaps()->scheduler;
    task_t*       caller       = resources->taskManager->GetCurrentTask();
    const int32_t previousNice = caller->baseNice;
    uint32_t      zero         = 0;
    fixture_store(process, base, &zero, sizeof(zero));
    fixture_store(process, base + 4, &zero, sizeof(zero));
    bool     ok     = expect(syscall_call(SYSCALL_futex, base, 136) == 0, "PI trylock acquires uncontended word");
    uint32_t actual = 0;
    fixture_load(process, base, &actual, sizeof(actual));
    ok &= expect((actual & 0x3fffffffU) == caller->id && syscall_call(SYSCALL_futex, base, 134) == static_cast<uint64_t>(-35), "PI owner TID and recursive deadlock rejection");
    ok &= expect(syscall_call(SYSCALL_futex, base, 135) == 0 && syscall_call(SYSCALL_futex, base, 135) == static_cast<uint64_t>(-1), "PI unlock checks ownership");
    ok &= expect(syscall_call(SYSCALL_futex, base, 134) == 0, "PI lock before userspace release");
    fixture_store(process, base, &zero, sizeof(zero));
    ok &= expect(syscall_call(SYSCALL_futex, base, 136) == 0, "PI reconciles uncontended userspace release");
    ok &= expect(syscall_call(SYSCALL_futex, base, 129, 1) == LINUX_EINVAL, "ordinary wake rejects tracked PI ownership");
    ok &= expect(syscall_call(SYSCALL_futex, base, 135) == 0, "reconciled PI owner unlocks");
    pi_test_context_t context     = {};
    context.scheduler             = scheduler;
    context.resources             = resources;
    context.first                 = base;
    context.second                = base + 4;
    task_t*           peers[3]    = {};
    bool              retained[3] = {};
    bool              attached[3] = {};
    arch_task_entry_t entries[]   = {pi_low_owner, pi_middle_owner, pi_medium_competitor};
    scheduler->SetNice(caller, -10);
    for (size_t i = 0; i < 3; ++i)
    {
        peers[i]    = resources->taskManager->CreateKernelTask(entries[i], &context);
        retained[i] = resources->taskManager->RetainForDispatch(peers[i]);
        attached[i] = retained[i] && resources->processManager->AddTask(process, peers[i]);
        ok &= expect(attached[i], "PI inversion peer allocation");
        if (!attached[i])
        {
            break;
        }
        scheduler->SetNice(peers[i], i == 0 ? 15 : i == 1 ? 5 : 0);
        if (i < 2)
        {
            scheduler->EnqueueProcess(arch_cpu_id(), process->id);
            ok &= expect(scheduler->WaitForEvent(&context.ready[i], 0, coordination_deadline()) == 0 && context.ready[i] == 1, "PI owner ready before contention");
        }
    }
    __atomic_store_n(&context.release, 1ULL, __ATOMIC_RELEASE);
    scheduler->WakeEventWaiters(&context.release);
    if (attached[2])
    {
        scheduler->EnqueueProcess(arch_cpu_id(), process->id);
        ok &= expect(syscall_call(SYSCALL_futex, base + 4, 134) == 0, "high-priority waiter acquires after transitive handoff");
        ok &= expect(context.inherited[0] && context.inherited[1] && context.restored[0] && context.restored[1] && !context.inverted && context.result[0] == 0 && context.result[1] == 0, "transitive PI runs both owners before medium competitor and restores priorities");
        ok &= expect(syscall_call(SYSCALL_futex, base + 4, 135) == 0, "PI final owner releases handoff");
    }
    while (context.done < 3 && attached[2])
    {
        const uint64_t done = context.done;
        if (scheduler->WaitForEvent(&context.done, done, coordination_deadline()) != 0)
        {
            ok = false;
            break;
        }
    }
    scheduler->SetNice(caller, previousNice);
    for (size_t i = 0; i < 3; ++i)
    {
        if (attached[i])
        {
            detach_fixture_peer(resources, process, peers[i]);
        }
        if (retained[i])
        {
            resources->taskManager->ReleaseDispatchClaim(peers[i]);
        }
        if (peers[i] != nullptr)
        {
            ok &= cleanup(resources->taskManager->FreeTask(peers[i]), "PI peer retirement");
        }
    }
    return ok;
}

struct requeue_pi_context_t
{
    ResourceLayerCaps* resources;
    Scheduler* scheduler;
    uint64_t source;
    uint64_t destination;
    uint64_t result;
    bool inherited;
    bool restored;
};

static void requeue_pi_owner(void* argument)
{
    cpu_preemption_disable();
    auto context = static_cast<requeue_pi_context_t*>(argument);
    task_t* task = context->resources->taskManager->GetCurrentTask();
    context->result = syscall_call(SYSCALL_futex, context->destination, 141);
    context->result |= syscall_call(SYSCALL_futex, context->source, 129, 1) == LINUX_EINVAL ? 0 : 1;
    context->result |= syscall_call(SYSCALL_futex, context->source, 140, 1, 1, context->destination, 8) == LINUX_EAGAIN ? 0 : 1;
    context->result |= syscall_call(SYSCALL_futex, context->source, 140, 1, 1, context->destination, 7) == 1 ? 0 : 1;
    context->inherited = task->scheduling->effectivePolicy == SCHEDULING_RR && task->scheduling->effectivePriority == 50;
    context->result |= syscall_call(SYSCALL_futex, context->destination, 135);
    context->restored = task->scheduling->effectivePolicy == SCHEDULING_OTHER;
    context->scheduler->ExitCurrentTask();
}

static bool test_requeue_pi(process_t* process, uint64_t base)
{
    auto dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto resources = dispatcher->GetResourceLayerCaps();
    auto scheduler = dispatcher->GetLogicLayerCaps()->scheduler;
    task_t* caller = resources->taskManager->GetCurrentTask();
    scheduling_attributes_t previous = {}, realtime = {};
    scheduler->ReadPolicy(caller, &previous);
    realtime.size = sizeof(realtime);
    realtime.policy = SCHEDULING_RR;
    realtime.priority = 50;
    uint32_t words[] = {7, 0};
    fixture_store(process, base, words, sizeof(words));
    requeue_pi_context_t context = {resources, scheduler, base, base + 4, UINT64_MAX};
    task_t* peer = resources->taskManager->CreateKernelTask(requeue_pi_owner, &context);
    const bool retained = resources->taskManager->RetainForDispatch(peer);
    const bool attached = retained && resources->processManager->AddTask(process, peer);
    bool ok = expect(attached, "allocate PI requeue owner");
    if (attached)
    {
        ok &= expect(scheduler->ConfigurePolicy(caller, realtime) == 0, "configure real-time inversion waiter");
        scheduler->EnqueueProcess(arch_cpu_id(), process->id);
        ok &= expect(syscall_call(SYSCALL_futex, base, 139, 7, 0, base + 4) == 0, "wait-requeue returns only after PI ownership acquired");
        uint32_t word = 0;
        fixture_load(process, base + 4, &word, 4);
        ok &= expect((word & 0x3fffffffU) == caller->id && context.result == 0 && context.inherited && context.restored, "atomic PI requeue donates RT priority and restores owner");
        ok &= expect(syscall_call(SYSCALL_futex, base + 4, 135) == 0, "requeued owner unlocks");
        ok &= expect(scheduler->ConfigurePolicy(caller, previous) == 0, "restore caller scheduling policy");
        detach_fixture_peer(resources, process, peer);
    }
    if (retained)
    {
        resources->taskManager->ReleaseDispatchClaim(peer);
    }
    if (peer != nullptr)
    {
        ok &= cleanup(resources->taskManager->FreeTask(peer), "retire PI requeue owner");
    }
    return ok;
}

static bool test_sync_futex(process_t* process)
{
    const uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    const uint64_t address = base + PAGE_SIZE - 4;
    const uint64_t timeout = base + 64;
    uint32_t       word    = 7;

    fixture_store(process, address, &word, sizeof(word));

    bool ok = expect(syscall_call(SYSCALL_futex, address, 128, 8) == (uint64_t) -11, "futex compare mismatch EAGAIN");

    ok &= test_sync_wake_operation(process, base);
    ok &= test_priority_futex(process, base + 256);
    ok &= test_pi_cleanup(process, base + 512);
    ok &= test_shared_pi(process);
    ok &= test_requeue_pi(process, base + 768);

    ok &= expect(syscall_call(SYSCALL_futex, address, 129, 1) == 0 && syscall_call(SYSCALL_futex, address, 129, 0) == 0, "futex wake with no waiters/zero count");

    ok &= expect(syscall_call(SYSCALL_futex, address + 1, 128, 7) == LINUX_EINVAL && syscall_call(SYSCALL_futex, CANONICAL_KERNEL_BASE, 128, 7) == LINUX_EFAULT &&
                     syscall_call(SYSCALL_futex, base + 2 * PAGE_SIZE, 128, 7) == LINUX_EFAULT,
        "futex alignment/kernel/unmapped word validation");

    ok &= expect(syscall_call(SYSCALL_futex, address, 137, 7, 0, 0, 0) == LINUX_EINVAL && syscall_call(SYSCALL_futex, address, 138, 1, 0, 0, 0) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_futex, address, 129, UINT64_MAX) == 0,
        "futex bitsets and empty legacy wake count");

    ok &= expect(syscall_call(SYSCALL_futex, address, 0, 7) == LINUX_EOPNOTSUPP && syscall_call(SYSCALL_futex, address, 139, 7) == LINUX_EINVAL && syscall_call(SYSCALL_futex, address, 128 | 256, 7) == LINUX_ENOSYS, "futex rejects unsupported backing, missing PI destination and invalid clock mode");

    ok &= expect(syscall_call(SYSCALL_futex, address, 131, 0, 1, address + 4) == 0 && syscall_call(SYSCALL_futex, address, 132, 0, 1, address + 4, 8) == (uint64_t) -11,
        "futex requeue empty queue and compare mismatch");

    ok &= expect(syscall_call(SYSCALL_futex, address, 132, 0, 1, address + 4, 7) == 0 && syscall_call(SYSCALL_futex, address, 131, 0, 0, address) == 0,
        "futex compare requeue and identical private keys");

    ok &= expect(syscall_call(SYSCALL_futex, address, 131, UINT64_MAX, 1, address + 4) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_futex, address, 132, 0, UINT64_MAX, address + 4, 7) == LINUX_EINVAL,
        "futex requeue negative counts");

    ok &= expect(syscall_call(SYSCALL_futex, address, 131, 0, 1, address + 1) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_futex, address, 132, 0, 1, CANONICAL_KERNEL_BASE, 7) == LINUX_EFAULT,
        "futex requeue destination validation");

    ok &= expect(syscall_call(SYSCALL_futex, address, 128, 7, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "futex invalid timeout buffer");

    time_timespec_t duration = {-1, 0};

    fixture_store(process, timeout, &duration, sizeof(duration));

    ok &= expect(syscall_call(SYSCALL_futex, address, 128, 7, timeout) == LINUX_EINVAL, "futex negative timeout");

    duration = {0, 1000000000};

    fixture_store(process, timeout, &duration, sizeof(duration));

    ok &= expect(syscall_call(SYSCALL_futex, address, 128, 7, timeout) == LINUX_EINVAL, "futex timeout nanoseconds");

    Dispatcher*        dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps* resources  = dispatcher->GetResourceLayerCaps();
    Scheduler*         scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    time_timespec_t    now;

    if (resources->timeManager != nullptr && resources->timeManager->ReadClock(1, &now) == 0)
    {
        duration = {0, 1000000};

        fixture_store(process, timeout, &duration, sizeof(duration));

        ok &= expect(syscall_call(SYSCALL_futex, address, 128, 7, timeout) == (uint64_t) -110, "futex relative finite timeout ETIMEDOUT");

        duration = {0, 0};

        fixture_store(process, timeout, &duration, sizeof(duration));

        ok &= expect(syscall_call(SYSCALL_futex, address, 137, 7, timeout, 0, 1) == (uint64_t) -110, "futex bitset absolute past deadline");

        ok &= expect(syscall_call(SYSCALL_futex, address, 128, 8, timeout) == (uint64_t) -11, "futex mismatch wins over expired timeout");

        ok &= expect(syscall_call(SYSCALL_futex, address, 129, 1) == 0, "futex timeout removes waiter");

        if (resources->timeManager->ReadClock(0, &now) == 0)
        {
            ok &= expect(syscall_call(SYSCALL_futex, address, 137 | 256, 7, timeout, 0, 1) == (uint64_t) -110, "futex bitset realtime absolute past deadline");
        }

        duration = {INT64_MAX, 0};

        fixture_store(process, timeout, &duration, sizeof(duration));

        ok &= expect(syscall_call(SYSCALL_futex, address, 128, 7, timeout) == LINUX_EOVERFLOW, "futex unrepresentable timeout");
    }
    else
    {
        kprintf("Arx kernel: futex timeout selftests unavailable without monotonic clock\n");
    }

    virt_addr_space_t* isolatedSpace = resources->virtualMemoryManager->CreateUserAddressSpace();
    process_t*         isolated      = isolatedSpace != nullptr ? resources->processManager->CreateProcess(isolatedSpace) : nullptr;

    if (isolated == nullptr)
    {
        if (isolatedSpace != nullptr)
        {
            resources->virtualMemoryManager->DestroyUserAddressSpace(isolatedSpace);
        }

        return expect(false, "futex private-key process isolation fixture");
    }

    const uint32_t waitOperations[] = {128U, 137U, 131U, 132U, 133U, 0x10085U, 0x20085U, 0x30085U, 0x40085U, 0x50085U};

    for (uint32_t operation : waitOperations)
    {
        word = 7;

        fixture_store(process, address, &word, sizeof(word));

        futex_peer_context_t context = {
            scheduler, resources, resources->taskManager->GetCurrentTask(), address, 2, operation == 137, false, false, UINT64_MAX, UINT64_MAX, UINT64_MAX, isolated, UINT64_MAX};
        const uint32_t command = operation & 0xffffU;

        context.operationMode = command >= 131 && command <= 133 ? command : 0;
        context.compareMode   = operation >> 16;

        task_t*    peer     = resources->taskManager->CreateKernelTask(futex_peer_entry, &context);
        const bool retained = resources->taskManager->RetainForDispatch(peer);
        const bool added    = retained && resources->processManager->AddTask(process, peer);
        const bool ready    = added && scheduler->EnqueueProcess(arch_cpu_id(), process->id);

        ok &= expect(ready, "futex runnable sibling task fixture");

        if (ready)
        {
            // Same process/address space: FUTEX_PRIVATE keys must not match a
            // different process simply because it maps the same virtual address.
            const uint32_t mask = operation == 128 ? UINT32_MAX : context.bits;
            const uint32_t wait = context.operationMode != 0 ? 137 : operation;

            const uint64_t result = syscall_call(SYSCALL_futex, address, wait, 7, 0, 0, mask);

            if (context.operationMode == 131 || context.operationMode == 132)
            {
                ok &= expect(result == 0 && context.entered && context.protectedTask && context.moved == 1 && context.wrongAddress == 0 && context.wrongBits == 0 &&
                                 context.result == 1,
                    "futex requeue moves parked waiter and preserves bitset");

                ok &= expect(context.mismatch == (operation == 132 ? (uint64_t) -11 : 0), "futex compare mismatch leaves waiter on source");
            }
            else if (context.operationMode == 133)
            {
                fixture_load(process, address, &word, sizeof(word));

                ok &= expect(result == 0 && context.mismatch == 0 && context.result == 1 && word == 11, "futex wake-op condition tests old value before atomic update");
            }
            else
            {
                ok &= expect(result == 0 && context.entered && context.protectedTask && context.wrongAddress == 0 && context.wrongProcess == 0 && context.result == 1,
                    "futex real parked wait/peer wake, process isolation and task lifetime");
            }

            if (operation == 137)
            {
                ok &= expect(context.wrongBits == 0, "futex bitset filters wakeups");
            }
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

            ok &= cleanup(resources->taskManager->FreeTask(peer), "futex peer cleanup");
        }

        ok &= expect(syscall_call(SYSCALL_futex, address, 129, 1) == 0, "futex wake removes waiter exactly once");
    }

    ok &= cleanup(resources->processManager->FreeProcess(isolated), "futex private-key process cleanup");

    resources->virtualMemoryManager->DestroyUserAddressSpace(isolatedSpace);

    ok &= test_sync_requeue_fifo(process, base);

    word = 7;

    fixture_store(process, address, &word, sizeof(word));

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_futex, address, 128, 8) == (uint64_t) -11, "futex compare permits readable readonly word");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 0);

    ok &= expect(syscall_call(SYSCALL_futex, address, 128, 7) == LINUX_EFAULT, "futex inaccessible word rejected");

    return ok;
}

static bool test_sync_robust(process_t* process, uint64_t number)
{
    const uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    const uint64_t headAddress   = base + PAGE_SIZE - 4;
    const uint64_t lengthAddress = base + 64;

    bool ok = expect(syscall_call(SYSCALL_set_robust_list, 0, 24) == 0, "robust empty-list clearing");

    ok &= expect(syscall_call(SYSCALL_set_robust_list, base, 23) == LINUX_EINVAL && syscall_call(SYSCALL_set_robust_list, base, 24) == 0,
        "robust length validation and registration");

    const uint64_t tid = syscall_call(SYSCALL_gettid);

    ok &= expect(syscall_call(SYSCALL_get_robust_list, number == SYSCALL_get_robust_list ? tid : 0, headAddress, lengthAddress) == 0, "robust current task query");

    uint64_t head = UINT64_MAX, length = 0;

    fixture_load(process, headAddress, &head, sizeof(head));

    fixture_load(process, lengthAddress, &length, sizeof(length));

    ok &= expect(head == base && length == 24, "robust registered head query");

    ok &= expect(syscall_call(SYSCALL_set_robust_list, 0, 24) == 0, "robust unregister releases owner pin");

    struct
    {
        uint64_t next;
        int64_t  offset;
        uint64_t pending;
    } robust = {base + 256, 8, base + 288};

    struct
    {
        uint64_t next;
        uint32_t word;
        uint32_t padding;
    } node = {base + 128, (uint32_t) tid | 0x80000000U, 0};

    fixture_store(process, base + 128, &robust, sizeof(robust));

    fixture_store(process, base + 256, &node, sizeof(node));

    fixture_store(process, base + 288, &node, sizeof(node));

    ok &= expect(syscall_call(SYSCALL_set_robust_list, base + 128, 24) == 0, "robust linked and pending registrations");
    static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->CleanupCurrentTask();
    uint32_t died = 0, pendingDied = 0;

    fixture_load(process, base + 264, &died, 4);

    fixture_load(process, base + 296, &pendingDied, 4);

    ok &= expect(died == 0xc0000000U && pendingDied == 0xc0000000U, "robust owner death updates list and pending operation");

    ok &= expect(syscall_call(SYSCALL_get_robust_list, 0, lengthAddress, lengthAddress) == 0, "robust aliased outputs permitted");

    fixture_load(process, lengthAddress, &head, sizeof(head));

    ok &= expect(head == 0, "robust outputs follow Linux length-then-head order");

    ok &= expect(syscall_call(SYSCALL_get_robust_list, UINT64_MAX, headAddress, lengthAddress) == LINUX_ESRCH &&
                     syscall_call(SYSCALL_get_robust_list, TaskManager::MAX_TASKS, headAddress, lengthAddress) == LINUX_ESRCH,
        "robust invalid task identifiers");

    ResourceLayerCaps* resources = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps();
    task_t*            sibling   = resources->taskManager->AllocateTask();
    const bool         attached  = sibling != nullptr && resources->processManager->AddTask(process, sibling);

    ok &= expect(attached, "robust sibling query fixture");

    if (attached)
    {
        __atomic_store_n(&sibling->robustHead, base + 128, __ATOMIC_RELEASE);

        ok &= expect(syscall_call(SYSCALL_get_robust_list, sibling->id, headAddress, lengthAddress) == 0, "robust sibling task query");

        fixture_load(process, headAddress, &head, sizeof(head));

        ok &= expect(head == base + 128 && sibling->dispatchUsers == 0, "robust sibling snapshot and lifetime claim release");

        __atomic_store_n(&sibling->robustHead, 0ULL, __ATOMIC_RELEASE);

        if (sibling->prev != nullptr)
        {
            sibling->prev->next = sibling->next;
        }
        else
        {
            process->tasks = sibling->next;
        }

        if (sibling->next != nullptr)
        {
            sibling->next->prev = sibling->prev;
        }

        sibling->next = sibling->prev = nullptr;
    }

    if (sibling != nullptr)
    {
        const uint64_t siblingId = sibling->id;

        ok &= cleanup(resources->taskManager->FreeTask(sibling), "robust sibling fixture cleanup");

        ok &= expect(syscall_call(SYSCALL_get_robust_list, siblingId, headAddress, lengthAddress) == LINUX_ESRCH, "robust retired sibling is absent");
    }

    ok &= expect(syscall_call(SYSCALL_get_robust_list, 0, CANONICAL_KERNEL_BASE, lengthAddress) == LINUX_EFAULT &&
                     syscall_call(SYSCALL_get_robust_list, 0, headAddress, CANONICAL_KERNEL_BASE) == LINUX_EFAULT,
        "robust invalid output pointers");

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_get_robust_list, 0, headAddress, lengthAddress) == LINUX_EFAULT, "robust readonly output across page");

    return ok;
}

template <uint64_t Number>
static bool validate_sync(const syscall_case_t*)
{
    return with_process([](process_t* process)
    {
        return Number == SYSCALL_futex ? test_sync_futex(process) : test_sync_robust(process, Number);
    });
}

void test_sync_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<SyncRequestManager> tests[] = {
        {"HandleFutexRequest", &SyncRequestManager::HandleFutexRequest, SYSCALL_futex, validate_sync<SYSCALL_futex>},
        {"HandleSet_robust_listRequest", &SyncRequestManager::HandleSet_robust_listRequest, SYSCALL_set_robust_list, validate_sync<SYSCALL_set_robust_list>},
        {"HandleGet_robust_listRequest", &SyncRequestManager::HandleGet_robust_listRequest, SYSCALL_get_robust_list, validate_sync<SYSCALL_get_robust_list>},
    };

    SyncRequestManager unavailable(nullptr, nullptr);

    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};

        if ((unavailable.*test.method)(&frame) != LINUX_ENOSYS || (caps->syncRequestManager != nullptr && (caps->syncRequestManager->*test.method)(nullptr) != LINUX_EINVAL))
        {
            expect(false, "sync missing dependency/null-frame validation");
        }
    }

    run_syscalls(caps->syncRequestManager, tests);
}

} // namespace request_selftest
