#include "RequestTestSupport.hpp"

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

    ok &= expect(syscall_call(SYSCALL_futex, address, 129, 1) == 0 && syscall_call(SYSCALL_futex, address, 129, 0) == 0, "futex wake with no waiters/zero count");

    ok &= expect(syscall_call(SYSCALL_futex, address + 1, 128, 7) == LINUX_EINVAL && syscall_call(SYSCALL_futex, CANONICAL_KERNEL_BASE, 128, 7) == LINUX_EFAULT &&
                     syscall_call(SYSCALL_futex, base + 2 * PAGE_SIZE, 128, 7) == LINUX_EFAULT,
        "futex alignment/kernel/unmapped word validation");

    ok &= expect(syscall_call(SYSCALL_futex, address, 137, 7, 0, 0, 0) == LINUX_EINVAL && syscall_call(SYSCALL_futex, address, 138, 1, 0, 0, 0) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_futex, address, 129, UINT64_MAX) == 0,
        "futex bitsets and empty legacy wake count");

    ok &= expect(syscall_call(SYSCALL_futex, address, 0, 7) == LINUX_EOPNOTSUPP && syscall_call(SYSCALL_futex, address, 134, 7) == LINUX_ENOSYS &&
                     syscall_call(SYSCALL_futex, address, 128 | 256, 7) == LINUX_ENOSYS,
        "futex shared/PI/unsupported clock modes explicit");

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
