#include "integrationtests.hpp"
#include "ScheduledFixture.hpp"
#include <selftests/PollBudget.hpp>
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/SynchronizationManager.hpp"
#include "layers/Logic/ProcessController.hpp"

namespace
{
constexpr uint64_t SharedSource = 0x50000000;
constexpr uint64_t SharedAlias  = 0x60000000;
process_t*         sharedPeer;
virt_addr_space_t* sharedPeerSpace;
unsigned           sharedOperation;

void shared_waiter(ScheduledFixture& f)
{
    const uint64_t mapped = f.L->processController->MapProcessMemory(SharedSource, 3 * PAGE_SIZE, 3, 0x21, -1, 0);

    if (!f.T.Equal(mapped, SharedSource, "map shared anonymous source"))
    {
        return;
    }

    auto* object    = f.R->virtualMemoryManager->FindRegion(f.Space, SharedSource)->object;
    sharedPeerSpace = f.R->virtualMemoryManager->CreateUserAddressSpace();

    if (!f.T.Check(sharedPeerSpace != nullptr, "create independent peer root"))
    {
        return;
    }

    sharedPeer = f.R->processManager->CreateProcess(sharedPeerSpace);

    if (!f.T.Check(sharedPeer != nullptr && f.R->processManager->AcquireProcess(sharedPeer->id) != nullptr, "retain independent peer process"))
    {
        return;
    }

    spinlock_acquire_preempt(&sharedPeerSpace->operation_lock);
    auto* unrelated = f.R->virtualMemoryManager->CreateBacking(PAGE_SIZE);
    bool mappedPeer = f.R->virtualMemoryManager->MapBackingPage(sharedPeerSpace, SharedAlias, object, PAGE_SIZE, 3, true, 7) &&
        f.R->virtualMemoryManager->MapBackingPage(sharedPeerSpace, SharedAlias + PAGE_SIZE, object, 2 * PAGE_SIZE, 3, true, 7) &&
        unrelated != nullptr && f.R->virtualMemoryManager->MapBackingPage(sharedPeerSpace, SharedSource + PAGE_SIZE, unrelated, 0, 3, true, 7);
    vmm_put_object(unrelated);
    spinlock_release_preempt(&sharedPeerSpace->operation_lock);

    if (!f.T.Check(mappedPeer, "map offset aliases and unrelated same-address page"))
    {
        return;
    }

    uint32_t value = 7;
    f.T.Check(safe_write_user(SharedSource + PAGE_SIZE, &value, sizeof(value)), "seed shared futex");
    cpu_preemption_disable();

    if (f.PublishPeer(sharedPeer))
    {
        f.Result = f.L->synchronizationManager->Wait(SharedSource + PAGE_SIZE, 7, 2, f.Deadline, true);
        f.T.Equal(f.Result, 0, "cross-process shared waiter wakes");
        f.T.Check(f.PeerDone, "independent peer completed");
    }

    cpu_preemption_enable();
}

void shared_waker(ScheduledFixture& f)
{
    auto* sync = f.L->synchronizationManager;
    f.T.Check(f.R->processManager->GetCurrentProcess() == sharedPeer, "waker uses distinct process and page table");
    f.T.Equal(sync->Wake(SharedAlias, 1, UINT32_MAX), 0, "private wake cannot consume shared key");
    f.T.Equal(sync->Wake(SharedAlias + 4, 1, UINT32_MAX, true), 0, "shared byte offsets distinguish words");
    f.T.Equal(sync->Wake(SharedAlias, 1, 1, true), 0, "shared bitset filters wake");
    f.T.Equal(sync->Wake(SharedSource + PAGE_SIZE, 1, UINT32_MAX, true), 0, "same virtual address with different shared backing stays isolated");

    if (sharedOperation == 1)
    {
        f.T.Equal(sync->Requeue(SharedAlias, SharedAlias + PAGE_SIZE, 0, 1, true, 8, true), static_cast<uint64_t>(-11), "shared compare mismatch preserves queue");
        f.T.Equal(sync->Requeue(SharedAlias, SharedAlias + PAGE_SIZE, 0, 1, true, 7, true), 1, "shared requeue transfers retained backing key");
        f.T.Equal(sync->Wake(SharedAlias, 1, UINT32_MAX, true), 0, "old shared key is empty after requeue");
        f.Word = sync->Wake(SharedAlias + PAGE_SIZE, 1, 2, true);
    }
    else if (sharedOperation == 2)
    {
        f.Word         = sync->WakeOperation(SharedAlias, SharedAlias + PAGE_SIZE, 1, 1, 0x00003000, true);
        uint32_t value = 0;
        f.T.Check(safe_read_user(SharedAlias + PAGE_SIZE, &value, sizeof(value)) && value == 3, "shared wake-op updates canonical destination");
    }
    else
    {
        spinlock_acquire_preempt(&f.Space->operation_lock);
        f.T.Equal(f.R->virtualMemoryManager->ReleaseAnonymousPage(f.Space, SharedSource + PAGE_SIZE), 0, "remove original waiter mapping while parked");
        spinlock_release_preempt(&f.Space->operation_lock);
        f.Word = sync->Wake(SharedAlias, 1, 2, true);
    }
}
}

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

static void sibling_remote(ScheduledFixture& fixture)
{
    const uint8_t cpu = arch_cpu_id();
    const uint8_t other = fixture.Main->ownerCpu;
    const bool simultaneous = other < platform.cpu_count && other != cpu && fixture.R->taskManager->GetRunningTask(other) == fixture.Main && fixture.R->processManager->GetRunningProcess(other) == fixture.Process && fixture.R->processManager->GetCurrentProcess() == fixture.Process;
    kfree(reinterpret_cast<void*>(fixture.Result));
    fixture.Result = reinterpret_cast<uint64_t>(kmalloc(128));
    __atomic_store_n(&fixture.Word, simultaneous ? 1U : 2U, __ATOMIC_RELEASE);
}

static void sibling_main(ScheduledFixture& fixture)
{
    cpu_preemption_disable();
    const uint8_t cpu = arch_cpu_id();
    uint8_t remote = (cpu + 1) % platform.cpu_count;
    for (size_t i = 0; i < platform.cpu_count; ++i)
    {
        if (platform.cpus[i].numa_node != platform.cpus[cpu].numa_node)
        {
            remote = i;
            break;
        }
    }
    fixture.T.Equal(fixture.L->scheduler->SetAffinity(fixture.Main, 1ULL << cpu), 0, "pin main sibling");
    fixture.T.Equal(fixture.L->scheduler->SetAffinity(fixture.Peer, 1ULL << remote), 0, "pin remote sibling");
    fixture.Result = reinterpret_cast<uint64_t>(kmalloc(128));
    fixture.T.Check(fixture.Result != 0, "allocate object before cross-node free");
    if (!fixture.PublishPeer())
    {
        kfree(reinterpret_cast<void*>(fixture.Result));
        return;
    }
    PollBudget budget;
    while (__atomic_load_n(&fixture.Word, __ATOMIC_ACQUIRE) == 0 || !fixture.Peer->dying || fixture.Peer->state == task_t::RUNNING || fixture.Peer->state == task_t::SWITCHING)
    {
        if (budget.Expired())
        {
            SelfTestManager::Get().AbortUnsafe("remote sibling did not execute and quiesce while main stayed running");
        }
        arch_pause();
    }
    fixture.T.Equal(fixture.Word, 1, "siblings execute concurrently in one process and address space");
    fixture.T.Check(fixture.Result != 0, "remote sibling allocates from its node heap");
    kfree(reinterpret_cast<void*>(fixture.Result));
}

void test_wait_integration(ResourceLayerCaps* r, LogicLayerCaps* l)
{
    {
        TestCase test("poststartkernel/scheduler/concurrent_siblings");
        if (platform.cpu_count < 2)
        {
            test.Skip("requires two CPUs");
        }
        else if (r != nullptr && l != nullptr && l->scheduler != nullptr)
        {
            uint64_t now = 0;
            arch_time_read_monotonic(&now);
            ScheduledFixture fixture(test, r, l, sibling_main, sibling_remote);
            fixture.Run(now + 1000000000);
        }
        else
        {
            test.Block("scheduler resources unavailable");
        }
    }

    if (r && l && l->scheduler && l->synchronizationManager && l->processController)
    {
        for (sharedOperation = 0; sharedOperation < 3; ++sharedOperation)
        {
            TestCase t(sharedOperation == 0 ? "poststartkernel/synchronization/shared_cross_process" : sharedOperation == 1 ? "poststartkernel/synchronization/shared_requeue" : "poststartkernel/synchronization/shared_wake_op");
            uint64_t now;

            if (!arch_time_read_monotonic(&now))
            {
                t.Skip("bounded wait requires monotonic clock");
                break;
            }

            sharedPeer      = nullptr;
            sharedPeerSpace = nullptr;
            {
                ScheduledFixture f(t, r, l, shared_waiter, shared_waker);
                f.Run(now + 1000000000);
                t.Equal(f.Word, 1, "exactly one shared waiter consumed");
                // Remove the independent process's queued retirement entry
                // before its process/task IDs can be reused by later cases.
                if (sharedPeer)
                {
                    (void) l->scheduler->KillProcess(sharedPeer->id, 0);
                }
            }

            if (sharedPeer)
            {
                r->processManager->ReleaseProcess(sharedPeer);
                t.Check(r->processManager->FreeProcess(sharedPeer), "reclaim independent peer process");
            }

            if (sharedPeerSpace)
            {
                r->virtualMemoryManager->DestroyUserAddressSpace(sharedPeerSpace);
            }
        }
    }
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
