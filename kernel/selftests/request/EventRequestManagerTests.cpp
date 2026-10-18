#include "RequestTestSupport.hpp"
#include "SignalAbiFixture.hpp"
#include <selftests/SocketFileSystemFixture.hpp>

namespace request_selftest
{
struct readiness_source_t
{
    inode_t  inode;
    uint32_t mask;
};

static uint64_t create_readiness_source(process_t* process, file_t** output, unsigned* closed)
{
    auto* dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto* vfs        = dispatcher->GetLogicLayerCaps()->virtualFileSystem;
    auto* resources  = dispatcher->GetResourceLayerCaps();
    static const file_operations_t operations = []()
    {
        file_operations_t result = {};
        result.Poll = [](file_t* file) -> uint32_t
        {
            return __atomic_load_n(&static_cast<readiness_source_t*>(file->privateData)->mask, __ATOMIC_ACQUIRE);
        };

        result.Release = [](file_t* file)
        {
            delete static_cast<readiness_source_t*>(file->privateData);
        };

        return result;
    }();

    auto* state = new readiness_source_t{};
    auto* file  = static_cast<file_t*>(kzalloc(sizeof(file_t)));

    if (state == nullptr || file == nullptr)
    {
        delete state;
        kfree(file);
        return LINUX_ENOMEM;
    }

    vfs->InitializeAnonymousInode(&state->inode, INODE_CHAR_DEVICE, 0600);
    file->inode                = &state->inode;
    file->privateData          = state;
    file->operations           = &operations;
    file->refCount             = 1;
    file->owner                = vfs;
    file->closeObserverContext = closed;
    file->closeObserver = [](void* context, file_t*)
    {
        ++*static_cast<unsigned*>(context);
    };

    const int64_t descriptor = resources->processManager->AddFileDescriptor(process, file, 0, dispatcher_vfs_file_operations());

    if (descriptor < 0)
    {
        vfs->Close(file);
        return (uint64_t) descriptor;
    }

    *output = file;
    return descriptor;
}

struct readiness_action_t
{
    void (*Run)(void* context);
    void*      context;
    Scheduler* scheduler;
    bool       finished;
    uint64_t   sequence;
};

static void readiness_action_entry(void* argument)
{
    cpu_preemption_disable();
    auto* action = static_cast<readiness_action_t*>(argument);
    action->Run(action->context);
    __atomic_store_n(&action->finished, true, __ATOMIC_RELEASE);
    __atomic_add_fetch(&action->sequence, 1ULL, __ATOMIC_RELEASE);
    action->scheduler->WakeEventWaiters(&action->sequence);
    action->scheduler->ExitCurrentTask();
}

template <typename Wait>
static bool with_readiness_action(process_t* process, readiness_action_t* action, Wait wait)
{
    auto* dispatcher    = static_cast<Dispatcher*>(platform.dispacher);
    auto* resources     = dispatcher->GetResourceLayerCaps();
    action->scheduler   = dispatcher->GetLogicLayerCaps()->scheduler;
    action->finished    = false;
    action->sequence    = 0;
    task_t*    peer     = resources->taskManager->CreateKernelTask(readiness_action_entry, action);
    const bool retained = resources->taskManager->RetainForDispatch(peer);
    const bool added    = retained && resources->processManager->AddTask(process, peer);
    const bool ready    = added && action->scheduler->EnqueueProcess(arch_cpu_id(), process->id);
    bool       ok       = expect(ready, "readiness interleaving peer fixture");

    if (ready)
    {
        ok &= wait();
        const uint64_t sequence = __atomic_load_n(&action->sequence, __ATOMIC_ACQUIRE);

        if (!__atomic_load_n(&action->finished, __ATOMIC_ACQUIRE))
        {
            ok &= expect(action->scheduler->WaitForEvent(&action->sequence, sequence, coordination_deadline()) == 0, "join readiness producer");
        }

        ok &= expect(__atomic_load_n(&action->finished, __ATOMIC_ACQUIRE), "readiness producer completed before waiter returned");
    }

    if (added)
    {
        detach_fixture_peer(resources, process, peer);
    }

    if (peer != nullptr)
    {
        if (retained)
        {
            resources->taskManager->ReleaseDispatchClaim(peer);
        }

        ok &= cleanup(resources->taskManager->FreeTask(peer), "readiness peer cleanup");
    }

    return ok;
}

static bool test_readiness_reuse(process_t* process, uint64_t base)
{
    auto*      dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto*      vfs        = dispatcher->GetLogicLayerCaps()->virtualFileSystem;
    bool       ok         = true;
    const bool modes[]    = {false, true};

    for (bool select : modes)
    {
        unsigned       closed      = 0;
        file_t*        original    = nullptr;
        const uint64_t watched     = create_readiness_source(process, &original, &closed);
        const uint64_t replacement = syscall_call(SYSCALL_eventfd2, 1, 04000);

        if ((int64_t) watched < 0 || watched >= 64 || (int64_t) replacement < 0)
        {
            if ((int64_t) watched >= 0)
            {
                syscall_call(SYSCALL_close, watched);
            }

            if ((int64_t) replacement >= 0)
            {
                syscall_call(SYSCALL_close, replacement);
            }

            return false;
        }

        struct context_t
        {
            VirtualFileSystem* vfs;
            file_t*            original;
            uint64_t           watched;
            uint64_t           replacement;
            uint64_t           result;
        } context = {vfs, original, watched, replacement, UINT64_MAX};

        // The peer owns a producer reference, independent of the descriptor.
        vfs->Retain(original);

        readiness_action_t action = {[](void* argument)
        {
            auto* context   = static_cast<context_t*>(argument);
            context->result = syscall_call(SYSCALL_dup2, context->replacement, context->watched);
            auto* source    = static_cast<readiness_source_t*>(context->original->privateData);
            __atomic_store_n(&source->mask, EVENT_POLL_PRI, __ATOMIC_RELEASE);
            context->vfs->NotifyReadiness(context->original, EVENT_POLL_PRI);
            context->vfs->Close(context->original);
        }, &context, nullptr, false};

        event_pollfd_t poll       = {(int32_t) watched, EVENT_POLL_PRI, 0};
        uint64_t       exceptions = 1ULL << watched;
        const int64_t  timeout[2] = {1, 0};
        fixture_store(process, base + 128, &poll, sizeof(poll));
        fixture_store(process, base + 256, &exceptions, sizeof(exceptions));
        fixture_store(process, base + 512, timeout, sizeof(timeout));

        ok &= with_readiness_action(process, &action, [&]()
        {
            const uint64_t result = select ? syscall_call(SYSCALL_select, watched + 1, 0, 0, base + 256, base + 512)
                                           : syscall_call(SYSCALL_poll, base + 128, 1, 1000);
            return expect(result == 1, "blocked poll/select retains original file across close and descriptor reuse");
        });

        fixture_load(process, base + 128, &poll, sizeof(poll));
        fixture_load(process, base + 256, &exceptions, sizeof(exceptions));
        ok &= expect(context.result == watched && closed == 1 && (select ? exceptions == (1ULL << watched) : poll.revents == EVENT_POLL_PRI),
            "reuse neither retargets readiness nor leaks the original provider");

        syscall_call(SYSCALL_close, watched);
        syscall_call(SYSCALL_close, replacement);
    }

    return ok;
}

static bool test_readiness_producers(process_t* process, uint64_t base)
{
    bool ok = true;

    for (uint32_t kind = 0; kind < 3; ++kind)
    {
        int32_t        pair[2] = {-1, -1};
        const uint64_t status  = kind == 0 ? syscall_call(SYSCALL_pipe2, base, 04000) : syscall_call(SYSCALL_socketpair, 1, (kind == 1 ? 1 : 2) | 04000, 0, base);

        if (!expect(status == 0, "concurrent pipe/socket source fixture"))
        {
            return false;
        }

        fixture_load(process, base, pair, sizeof(pair));
        const uint64_t epfd = syscall_call(SYSCALL_epoll_create1, 0);
        const uint64_t ack  = syscall_call(SYSCALL_eventfd2, 0, 1);

        if ((int64_t) epfd < 0 || (int64_t) ack < 0)
        {
            syscall_call(SYSCALL_close, pair[0]);
            syscall_call(SYSCALL_close, pair[1]);

            if ((int64_t) epfd >= 0)
            {
                syscall_call(SYSCALL_close, epfd);
            }

            if ((int64_t) ack >= 0)
            {
                syscall_call(SYSCALL_close, ack);
            }

            return false;
        }

        event_epoll_event_t event = {EVENT_POLL_IN | 0xc0000000U, kind};
        const uint64_t      one   = 1;
        fixture_store(process, base + 128, &one, sizeof(one));
        fixture_store(process, base + 512, &one, sizeof(one));
        fixture_store(process, base + 768, &event, sizeof(event));
        ok &= expect(syscall_call(SYSCALL_epoll_ctl, epfd, 1, pair[0], base + 768) == 0, "edge and one-shot registration for concurrent source");
        struct context_t
        {
            int32_t  writer;
            uint64_t ack;
            uint64_t base;
            bool     cancel;
            bool     passed;
        } context = {pair[1], ack, base, false, true};
        readiness_action_t action = {[](void* argument)
        {
            auto* context = static_cast<context_t*>(argument);

            for (size_t i = 0; i < 32 && !__atomic_load_n(&context->cancel, __ATOMIC_ACQUIRE); ++i)
            {
                if (syscall_call(SYSCALL_write, context->writer, context->base + 128, 1) != 1 || syscall_call(SYSCALL_read, context->ack, context->base + 1024, 8) != 8)
                {
                    context->passed = false;
                    break;
                }
            }
        }, &context, nullptr, false, 0};
        ok &= with_readiness_action(process, &action, [&]()
        {
            bool passed = true;

            for (size_t i = 0; i < 32; ++i)
            {
                if (syscall_call(SYSCALL_epoll_wait, epfd, base + 1536, 1, 1000) != 1 || syscall_call(SYSCALL_read, pair[0], base + 256, 1) != 1 ||
                    syscall_call(SYSCALL_epoll_ctl, epfd, 3, pair[0], base + 768) != 0)
                {
                    __atomic_store_n(&context.cancel, true, __ATOMIC_RELEASE);
                    passed = false;
                }

                syscall_call(SYSCALL_write, ack, base + 512, 8);

                if (!passed)
                {
                    break;
                }
            }

            return expect(passed, "concurrent drain/refill and one-shot rearm never lose pipe/socket wakeups");
        });
        ok &= expect(context.passed, "concurrent producer completed every acknowledged write");
        syscall_call(SYSCALL_close, pair[0]);
        syscall_call(SYSCALL_close, pair[1]);
        syscall_call(SYSCALL_close, ack);
        syscall_call(SYSCALL_close, epfd);
    }

    return ok;
}

static bool test_readiness_interruption(process_t* process, uint64_t base)
{
    auto*                 dispatcher        = static_cast<Dispatcher*>(platform.dispacher);
    auto*                 signals           = dispatcher->GetLogicLayerCaps()->signalManager;
    auto*                 task              = dispatcher->GetResourceLayerCaps()->taskManager->GetCurrentTask();
    const uint64_t        oldMask           = task->signalMask;
    const uint64_t        oldPending        = task->signalPending;
    const uint64_t        oldProcessPending = process->signalPending;
    signal_action_t       previous          = {};
    const signal_action_t handler           = {base + 1280, 0x04000000, base + 1536, 0};

    if (!expect(signals->Action(10, &handler, &previous) == 0, "blocking interruption signal action"))
    {
        return false;
    }

    __atomic_store_n(&task->signalMask, oldMask & ~(1ULL << 9), __ATOMIC_RELEASE);
    bool ok = true;

    for (uint32_t kind = 0; kind < 3; ++kind)
    {
        __atomic_store_n(&task->signalPending, 0ULL, __ATOMIC_RELEASE);
        __atomic_store_n(&process->signalPending, 0ULL, __ATOMIC_RELEASE);
        const uint64_t epfd = syscall_call(SYSCALL_epoll_create1, 0);

        if ((int64_t) epfd < 0)
        {
            ok = false;
            break;
        }

        struct context_t
        {
            SignalManager* signals;
            int32_t        pid;
            int32_t        tid;
            uint64_t       result;
        } context = {signals, (int32_t) process->id, -1, UINT64_MAX};

        readiness_action_t action = {[](void* argument)
        {
            auto* context   = static_cast<context_t*>(argument);
            context->result = context->signals->Send(context->pid, context->tid, 10);
        }, &context, nullptr, false, 0};

        const int64_t duration[2] = {1, 0};
        fixture_store(process, base + 512, duration, sizeof(duration));

        ok &= with_readiness_action(process, &action, [&]()
        {
            const uint64_t result = kind == 0 ? syscall_call(SYSCALL_poll, 0, 0, 1000)
                                             : kind == 1 ? syscall_call(SYSCALL_select, 0, 0, 0, 0, base + 512)
                                                         : syscall_call(SYSCALL_epoll_wait, epfd, base + 256, 1, 1000);
            __atomic_store_n(&task->signalPending, 0ULL, __ATOMIC_RELEASE);
            __atomic_store_n(&process->signalPending, 0ULL, __ATOMIC_RELEASE);
            return expect(result == (uint64_t) -4, "real queued signal interrupts a parked poll/select/epoll waiter");
        });

        ok &= expect(context.result == 0, "signal producer targets the waiting process");

        syscall_call(SYSCALL_close, epfd);
    }

    signals->Action(10, &previous, nullptr);
    __atomic_store_n(&task->signalMask, oldMask, __ATOMIC_RELEASE);
    __atomic_store_n(&task->signalPending, oldPending, __ATOMIC_RELEASE);
    __atomic_store_n(&process->signalPending, oldProcessPending, __ATOMIC_RELEASE);

    return ok;
}

static bool test_epoll_exclusive(process_t* process, uint64_t base)
{
    auto*          dispatcher  = static_cast<Dispatcher*>(platform.dispacher);
    auto*          vfs         = dispatcher->GetLogicLayerCaps()->virtualFileSystem;
    unsigned       closed      = 0;
    file_t*        file        = nullptr;
    const uint64_t source      = create_readiness_source(process, &file, &closed);
    const uint64_t instances[] = {syscall_call(SYSCALL_epoll_create1, 0), syscall_call(SYSCALL_epoll_create1, 0), syscall_call(SYSCALL_epoll_create1, 0)};
    bool ok = expect((int64_t) source >= 0 && (int64_t) instances[0] >= 0 && (int64_t) instances[1] >= 0 && (int64_t) instances[2] >= 0,
        "exclusive epoll source and instance allocation");

    if (ok)
    {
        inode_t unsupportedInode = {};
        file_t  unsupportedFile  = {};
        unsupportedFile.inode    = &unsupportedInode;
        unsupportedInode.type    = INODE_CHAR_DEVICE;
        ok &= expect(vfs->Poll(&unsupportedFile) == EVENT_POLL_ERR, "future device without a readiness provider is not reported synchronously ready");
        unsupportedInode.type = INODE_REGULAR;
        ok &= expect((vfs->Poll(&unsupportedFile) & (EVENT_POLL_IN | EVENT_POLL_OUT)) == (EVENT_POLL_IN | EVENT_POLL_OUT), "regular file readiness defaults are preserved");

        for (size_t i = 0; i < 3; ++i)
        {
            event_epoll_event_t event = {EVENT_POLL_IN | (i != 2 ? 0x10000000U : 0), i};
            fixture_store(process, base + 128, &event, sizeof(event));
            ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[i], 1, source, base + 128) == 0, "exclusive and nonexclusive registrations coexist with legacy close observer");
            ok &= expect(syscall_call(SYSCALL_epoll_wait, instances[i], base + 256, 1, 0) == 0, "empty source clears initial registration readiness");
        }

        bool selected[2] = {};

        for (size_t iteration = 0; iteration < 4; ++iteration)
        {
            __atomic_store_n(&static_cast<readiness_source_t*>(file->privateData)->mask, EVENT_POLL_IN, __ATOMIC_RELEASE);
            vfs->NotifyReadiness(file, EVENT_POLL_IN);
            uint64_t exclusiveReady = 0;

            for (size_t i = 0; i < 2; ++i)
            {
                const uint64_t result = syscall_call(SYSCALL_epoll_wait, instances[i], base + 256, 1, 0);
                selected[i] |= result == 1;
                exclusiveReady += result;
            }

            ok &= expect(exclusiveReady == 1 && syscall_call(SYSCALL_epoll_wait, instances[2], base + 256, 1, 0) == 1,
                "producer selects one exclusive instance and every nonexclusive instance");
            __atomic_store_n(&static_cast<readiness_source_t*>(file->privateData)->mask, 0U, __ATOMIC_RELEASE);

            for (uint64_t instance : instances)
            {
                ok &= expect(syscall_call(SYSCALL_epoll_wait, instance, base + 256, 1, 0) == 0, "drained exclusive source is not ready");
            }
        }

        ok &= expect(selected[0] && selected[1], "exclusive selection does not permanently starve either instance");
        event_epoll_event_t event = {EVENT_POLL_IN, 0};
        fixture_store(process, base + 128, &event, sizeof(event));
        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[0], 3, source, base + 128) == LINUX_EINVAL, "exclusive registration cannot be modified");
        event.events = EVENT_POLL_IN | 0x10000000U;
        fixture_store(process, base + 128, &event, sizeof(event));
        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[0], 1, instances[2], base + 128) == LINUX_EINVAL, "exclusive nested registration is invalid");
        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[0], 2, source, 0) == 0, "DEL removes the intrusive source observer");
        event.events |= 0x40000000U;
        fixture_store(process, base + 128, &event, sizeof(event));
        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[0], 1, source, base + 128) == LINUX_EINVAL, "exclusive one-shot registration is invalid");
        event.events = EVENT_POLL_IN | 0x10000000U | 0x80000000U;
        fixture_store(process, base + 128, &event, sizeof(event));
        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[0], 1, source, base + 128) == 0 && syscall_call(SYSCALL_epoll_ctl, instances[0], 2, source, 0) == 0,
            "exclusive edge registration is supported and removable");
        syscall_call(SYSCALL_close, source);
        ok &= expect(closed == 1, "source final close invokes its original observer exactly once");

        for (uint64_t instance : instances)
        {
            ok &= expect(syscall_call(SYSCALL_epoll_wait, instance, base + 256, 1, 0) == 0, "source final close detaches every remaining weak watch");
        }
    }
    else if ((int64_t) source >= 0)
    {
        syscall_call(SYSCALL_close, source);
    }

    for (uint64_t instance : instances)
    {
        if ((int64_t) instance >= 0)
        {
            syscall_call(SYSCALL_close, instance);
        }
    }

    return ok;
}

struct event_peer_context_t
{
    ResourceLayerCaps* resources;
    Scheduler*         scheduler;
    process_t*         sleeper;
    task_t*            sleepingTask;
    uint64_t           descriptor;
    bool               read;
    bool               entered;
    bool               protectedLifetime;
    uint64_t           result;
};

static void event_peer_entry(void* argument)
{
    cpu_preemption_disable();

    event_peer_context_t* context = static_cast<event_peer_context_t*>(argument);

    context->entered           = true;
    context->protectedLifetime = !context->resources->taskManager->FreeTask(context->sleepingTask) && !context->resources->processManager->FreeProcess(context->sleeper) &&
                                 !context->resources->taskManager->ExecuteTask(context->sleepingTask);
    {
        request_user_fixture_t fixture;
        uint64_t               value = 5;
        uint64_t               user  = fixture.Address(&value, sizeof(value));

        context->result = syscall_call(context->read ? SYSCALL_read : SYSCALL_write, context->descriptor, user, sizeof(value));
    }

    // Leave process teardown to the fixture. Publishing exited here lets idle
    // reapers race its descriptor-reference and process cleanup assertions.
    context->scheduler->ExitCurrentTask();
}

template <typename Wait>
static bool event_with_peer(process_t* process, uint64_t fd, bool peerReads, Wait wait)
{
    Dispatcher*          dispatcher       = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps*   resources        = dispatcher->GetResourceLayerCaps();
    Scheduler*           scheduler        = dispatcher->GetLogicLayerCaps()->scheduler;
    VirtualFileSystem*   vfs              = dispatcher->GetLogicLayerCaps()->virtualFileSystem;
    event_peer_context_t context          = {resources, scheduler, process, resources->taskManager->GetCurrentTask(), 0, peerReads, false, false, LINUX_ENOSYS};
    virt_addr_space_t*   space            = resources->virtualMemoryManager->CreateUserAddressSpace();
    process_t*           peer             = space != nullptr ? resources->processManager->CreateProcess(space) : nullptr;
    task_t*              task             = resources->taskManager->CreateKernelTask(event_peer_entry, &context);
    file_t*              sharedFile       = nullptr;
    uint64_t             referencesBefore = 0;

    // Keep the task slot stable until the fixture detaches and frees it.
    const bool retained = resources->taskManager->RetainForDispatch(task);
    bool       ready    = peer != nullptr && retained;

    if (ready)
    {
        FileDescriptorGuard guard(process);
        file_descriptor_t*  source = resources->processManager->GetFileDescriptor(process, fd);
        file_t*             file   = source != nullptr ? static_cast<file_t*>(source->file) : nullptr;

        ready = file != nullptr && vfs->Retain(file);

        if (ready)
        {
            sharedFile = file;

            spinlock_acquire_preempt(&file->refLock);

            referencesBefore = file->refCount - 1;

            spinlock_release_preempt(&file->refLock);

            int64_t shared = resources->processManager->AddFileDescriptor(peer, file, 0, dispatcher_vfs_file_operations());

            if (shared < 0)
            {
                vfs->Close(file);
            }
            else
            {
                context.descriptor = (uint64_t) shared;
            }

            ready = shared >= 0;
        }
    }

    const bool added = ready && resources->processManager->AddTask(peer, task);

    ready = added && scheduler->EnqueueProcess(arch_cpu_id(), peer->id);

    bool ok = expect(ready, "event runnable producer/consumer fixture");

    if (ready)
    {
        ok &= wait();

        ok &= expect(context.entered && context.protectedLifetime && context.result == 8, "event wake executes peer and protects parked task/process");
    }

    if (added)
    {
        resources->processManager->RemoveTask(peer, task);
    }

    if (task != nullptr)
    {
        if (retained)
        {
            resources->taskManager->ReleaseDispatchClaim(task);
        }

        ok &= cleanup(resources->taskManager->FreeTask(task), "event peer task cleanup");
    }

    if (peer != nullptr)
    {
        ok &= cleanup(resources->processManager->FreeProcess(peer), "event peer process cleanup");
    }

    if (sharedFile != nullptr)
    {
        spinlock_acquire_preempt(&sharedFile->refLock);

        const bool releasedReference = sharedFile->refCount == referencesBefore;

        spinlock_release_preempt(&sharedFile->refLock);

        ok &= expect(releasedReference, "process teardown releases shared eventfd reference");
    }

    if (space != nullptr)
    {
        resources->virtualMemoryManager->DestroyUserAddressSpace(space);
    }

    return ok;
}

static bool test_event_counter_vector(process_t* process, uint64_t fd, uint64_t base)
{
    const uint64_t metadata   = base + 384;
    const uint64_t first      = base + 256;
    const uint64_t second     = base + PAGE_SIZE + 64;
    uint64_t       values[2]  = {5, 7};
    file_iovec_t   vectors[3] = {{base + 64, 8}, {base + 72, 8}, {0, 0}};

    fixture_store(process, base + 64, values, sizeof(values));

    fixture_store(process, metadata, vectors, sizeof(vectors));

    bool ok = expect(syscall_call(SYSCALL_writev, fd, metadata, 3) == 16, "eventfd writev increments once per eight-byte vector");

    vectors[0] = {0, 0};
    vectors[1] = {first, 3};
    vectors[2] = {second, 13};

    uint8_t untouched[13];

    memset(untouched, 0xcc, sizeof(untouched));

    fixture_store(process, second, untouched, sizeof(untouched));

    fixture_store(process, metadata, vectors, sizeof(vectors));

    ok &= expect(syscall_call(SYSCALL_readv, fd, metadata, 3) == 8, "eventfd readv scatters one eight-byte counter");

    uint64_t actual = 0;

    fixture_load(process, first, &actual, 3);

    fixture_load(process, second, reinterpret_cast<uint8_t*>(&actual) + 3, 5);

    fixture_load(process, second, untouched, sizeof(untouched));

    ok &= expect(actual == 12 && untouched[5] == 0xcc && untouched[12] == 0xcc, "eventfd readv preserves bytes beyond counter");

    ok &= expect(syscall_call(SYSCALL_readv, fd, metadata, 3) == (uint64_t) -11, "empty eventfd readv EAGAIN");

    vectors[0] = {base + 64, 4};
    vectors[1] = {base + 68, 4};

    fixture_store(process, metadata, vectors, sizeof(vectors));

    ok &= expect(syscall_call(SYSCALL_writev, fd, metadata, 2) == LINUX_EINVAL, "eventfd writev does not combine short increments");

    vectors[0] = {base + 64, 8};

    fixture_store(process, metadata, vectors, sizeof(vectors));

    ok &= expect(syscall_call(SYSCALL_writev, fd, metadata, 2) == 8, "eventfd writev returns progress before invalid later increment");

    ok &= expect(syscall_call(SYSCALL_read, fd, first, 8) == 8, "eventfd partial writev counter readback");

    fixture_load(process, first, &actual, sizeof(actual));

    ok &= expect(actual == 5, "eventfd partial writev commits only successful increment");

    vectors[0] = {first, 3};
    vectors[1] = {second, 5};

    fixture_store(process, metadata, vectors, sizeof(vectors));

    syscall_call(SYSCALL_write, fd, base + 64, 8);

    const uint64_t sentinel = UINT64_MAX;

    fixture_store(process, first, &sentinel, sizeof(sentinel));

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_readv, fd, metadata, 2) == LINUX_EFAULT, "eventfd readv validates every output segment before scatter");

    fixture_load(process, first, &actual, sizeof(actual));

    ok &= expect(actual == sentinel, "eventfd failed scatter preserves earlier output segment");

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);

    ok &= expect(syscall_call(SYSCALL_readv, fd, metadata, 2) == 8, "eventfd failed scatter preserves counter");

    vectors[1].length = 4;

    fixture_store(process, metadata, vectors, sizeof(vectors));

    ok &= expect(syscall_call(SYSCALL_readv, fd, metadata, 2) == LINUX_EINVAL, "eventfd short aggregate readv rejected");

    ok &= expect(syscall_call(SYSCALL_readv, fd, CANONICAL_KERNEL_BASE, 1) == LINUX_EFAULT && syscall_call(SYSCALL_readv, fd, metadata, 1025) == LINUX_EINVAL,
        "eventfd vector metadata validation");

    ok &= expect(syscall_call(SYSCALL_readv, fd, CANONICAL_KERNEL_BASE, 0) == 0 && syscall_call(SYSCALL_writev, fd, CANONICAL_KERNEL_BASE, 0) == 0,
        "eventfd zero vector count ignores pointer");

    syscall_call(SYSCALL_fcntl, fd, 4, 2);

    vectors[1].length = 5;

    fixture_store(process, metadata, vectors, sizeof(vectors));

    ok &= event_with_peer(process, fd, false, [&]()
    {
        return expect(syscall_call(SYSCALL_readv, fd, metadata, 2) == 8, "parked eventfd readv wakes on peer write");
    });
    values[0] = UINT64_MAX - 1;

    fixture_store(process, base + 64, values, sizeof(values));

    syscall_call(SYSCALL_write, fd, base + 64, 8);

    values[0] = 1;

    fixture_store(process, base + 64, values, sizeof(values));

    vectors[0] = {base + 64, 8};

    fixture_store(process, metadata, vectors, sizeof(vectors));

    ok &= event_with_peer(process, fd, true, [&]()
    {
        return expect(syscall_call(SYSCALL_writev, fd, metadata, 1) == 8, "parked eventfd writev wakes on peer read");
    });
    syscall_call(SYSCALL_fcntl, fd, 4, 2 | 04000);

    ok &= expect(syscall_call(SYSCALL_read, fd, first, 8) == 8, "eventfd vector fixture drain");

    return ok;
}

static bool test_event_counter(process_t* process, uint64_t number)
{
    const uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    uint64_t fd = number == SYSCALL_eventfd ? syscall_call(number, 3) : syscall_call(number, 3, 04000 | 02000000);

    if ((int64_t) fd < 0)
    {
        return expect(false, "eventfd creates descriptor");
    }

    bool ok = expect(syscall_call(SYSCALL_fcntl, fd, 1) == (number == SYSCALL_eventfd2 ? 1ULL : 0ULL), "eventfd CLOEXEC descriptor flag");

    ok &= expect(syscall_call(SYSCALL_fcntl, fd, 4, 2 | 04000) == 0, "eventfd nonblocking status through fcntl");

    ok &= expect(syscall_call(SYSCALL_fcntl, fd, 4, 04000) == 0 && syscall_call(SYSCALL_fcntl, fd, 3) == (2 | 04000), "eventfd SETFL preserves access mode");

    const uint64_t output = base + PAGE_SIZE - 4;

    ok &= expect(syscall_call(SYSCALL_read, fd, output, 16) == 8, "eventfd read exact eight-byte result across page boundary");

    uint64_t value = 0;

    fixture_load(process, output, &value, sizeof(value));

    ok &= expect(value == 3 && syscall_call(SYSCALL_read, fd, output, 8) == (uint64_t) -11, "eventfd reset-on-read and empty EAGAIN");

    ok &= expect(syscall_call(SYSCALL_read, fd, output, 0) == LINUX_EINVAL && syscall_call(SYSCALL_write, fd, base, 7) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_write, fd, base, 9) == LINUX_EINVAL,
        "eventfd validates transfer sizes");

    value = UINT64_MAX;

    fixture_store(process, base, &value, sizeof(value));

    ok &= expect(syscall_call(SYSCALL_write, fd, base, 8) == LINUX_EINVAL, "eventfd rejects UINT64_MAX");

    value = UINT64_MAX - 1;

    fixture_store(process, base, &value, sizeof(value));

    ok &= expect(syscall_call(SYSCALL_write, fd, base, 8) == 8, "eventfd reaches maximum counter");

    value = 1;

    fixture_store(process, base, &value, sizeof(value));

    ok &= expect(syscall_call(SYSCALL_write, fd, base, 8) == (uint64_t) -11, "eventfd overflow EAGAIN");

    ok &= expect(syscall_call(SYSCALL_read, fd, CANONICAL_KERNEL_BASE, 8) == LINUX_EFAULT, "eventfd rejects kernel read output");

    ok &= expect(syscall_call(SYSCALL_read, fd, output, 8) == 8, "eventfd bad output leaves counter intact");

    fixture_load(process, output, &value, sizeof(value));

    ok &= expect(value == UINT64_MAX - 1, "eventfd full counter roundtrip");

    value = 7;

    fixture_store(process, base, &value, sizeof(value));

    ok &= expect(syscall_call(SYSCALL_write, fd, base, 8) == 8, "eventfd shared-reference setup");

    uint64_t duplicate = syscall_call(SYSCALL_dup, fd);

    ok &= expect((int64_t) duplicate >= 0 && syscall_call(SYSCALL_close, fd) == 0, "eventfd dup survives original close");

    if ((int64_t) duplicate >= 0)
    {
        fd = duplicate;

        ok &= expect(syscall_call(SYSCALL_read, fd, output, 8) == 8, "eventfd duplicate shares counter backing");

        fixture_load(process, output, &value, sizeof(value));

        ok &= expect(value == 7, "eventfd duplicate value");
    }

    ok &= expect(syscall_call(SYSCALL_lseek, fd, 123, 0) == 0, "eventfd no-op seek");

    uint64_t vector[2] = {base, 8};

    fixture_store(process, base + 128, vector, sizeof(vector));

    ok &= expect(syscall_call(SYSCALL_readv, fd, base + 128, 1) == (uint64_t) -11 && syscall_call(SYSCALL_pread64, fd, base, 8, 0) == (uint64_t) -29,
        "eventfd empty vector read and unsupported positioned I/O");

    ok &= test_event_counter_vector(process, fd, base);

    // Clear NONBLOCK and exercise an actual parked read with another process.
    syscall_call(SYSCALL_fcntl, fd, 4, 2);

    ok &= event_with_peer(process, fd, false, [&]()
    {
        return expect(syscall_call(SYSCALL_read, fd, output, 8) == 8, "blocking eventfd read wakes on peer write");
    });
    value = UINT64_MAX - 1;

    fixture_store(process, base, &value, sizeof(value));

    ok &= expect(syscall_call(SYSCALL_write, fd, base, 8) == 8, "blocking write full-counter setup");

    value = 1;

    fixture_store(process, base, &value, sizeof(value));

    ok &= event_with_peer(process, fd, true, [&]()
    {
        return expect(syscall_call(SYSCALL_write, fd, base, 8) == 8, "blocking eventfd write wakes on peer read");
    });
    syscall_call(SYSCALL_fcntl, fd, 4, 2 | 04000);

    ok &= expect(syscall_call(SYSCALL_read, fd, output, 8) == 8, "blocking write leaves written increment");

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

    value = 4;

    fixture_store(process, base, &value, sizeof(value));

    syscall_call(SYSCALL_write, fd, base, 8);

    ok &= expect(syscall_call(SYSCALL_read, fd, output, 8) == LINUX_EFAULT, "eventfd readonly output");

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);

    ok &= expect(syscall_call(SYSCALL_read, fd, output, 8) == 8, "eventfd readonly failure preserves counter");

    ok &= expect(syscall_call(SYSCALL_write, fd, CANONICAL_KERNEL_BASE, 8) == LINUX_EFAULT && syscall_call(SYSCALL_write, fd, base + 2 * PAGE_SIZE - 4, 8) == LINUX_EFAULT,
        "eventfd invalid input buffers");

    ok &= expect(syscall_call(SYSCALL_close, fd) == 0, "eventfd final close");

    if (number == SYSCALL_eventfd2)
    {
        ok &= expect(syscall_call(number, 0, 2) == LINUX_EINVAL, "eventfd2 invalid flags");

        uint64_t semaphore = syscall_call(number, 2, 1 | 04000);

        ok &= expect((int64_t) semaphore >= 0, "eventfd2 semaphore creation");

        if ((int64_t) semaphore >= 0)
        {
            const file_iovec_t vectors[2] = {{base, 3}, {base + 3, 5}};

            fixture_store(process, base + 384, vectors, sizeof(vectors));

            ok &= expect(syscall_call(SYSCALL_readv, semaphore, base + 384, 2) == 8, "eventfd semaphore scatter read");

            fixture_load(process, base, &value, sizeof(value));

            ok &= expect(value == 1 && syscall_call(SYSCALL_read, semaphore, base, 8) == 8 && syscall_call(SYSCALL_read, semaphore, base, 8) == (uint64_t) -11,
                "eventfd semaphore decrements by one");

            syscall_call(SYSCALL_close, semaphore);
        }
    }

    Dispatcher*          dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ProcessManager*      processes  = dispatcher->GetResourceLayerCaps()->processManager;
    process_file_limit_t old        = {}, limit = {0, process->fileLimit.maximum};

    processes->ChangeFileLimit(process, &limit, &old);

    ok &= expect(syscall_call(number, 0, 0) == LINUX_EMFILE, "eventfd descriptor limit rollback");

    processes->ChangeFileLimit(process, &old, nullptr);

    return ok;
}

static bool test_event_temporary_mask(process_t* process, uint64_t number, uint64_t base, uint64_t epfd = 0)
{
    Dispatcher*        dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps* resources  = dispatcher->GetResourceLayerCaps();
    SignalManager*     signals    = dispatcher->GetLogicLayerCaps()->signalManager;
    task_t*            task       = resources->taskManager->GetCurrentTask();

    const uint64_t originalMask    = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);
    const uint64_t originalPending = __atomic_load_n(&task->signalPending, __ATOMIC_ACQUIRE);
    const uint64_t originalSaved   = task->signalSavedMask;
    const bool     originalRestore = task->signalRestoreMask;
    const uint64_t originalDepth   = task->signalDepth;
    const uint64_t processPending  = __atomic_load_n(&process->signalPending, __ATOMIC_ACQUIRE);
    const uint64_t blockedMask     = (1ULL << 9) | (1ULL << 11);

    signal_action_t previous  = {};
    signal_action_t action    = {base + 1280, 0x04000000, base + 1536, 0};
    const uint64_t  installed = signals->Action(10, &action, &previous);

    if (!expect(installed == 0, "event signal handler fixture"))
    {
        return false;
    }

    __atomic_store_n(&task->signalMask, blockedMask, __ATOMIC_RELEASE);

    uint64_t mask      = 0;
    uint64_t bundle[2] = {base + 1024, sizeof(mask)};

    fixture_store(process, base + 1024, &mask, sizeof(mask));

    fixture_store(process, base + 1056, bundle, sizeof(bundle));

    arch_syscall_frame_t frame = {};

    frame.syscall_number = number;

    if (number == SYSCALL_ppoll)
    {
        frame.arg3 = base + 1024;
        frame.arg4 = sizeof(mask);
    }
    else if (number == SYSCALL_pselect6)
    {
        frame.arg5 = base + 1056;
    }
    else
    {
        frame.arg0 = epfd;
        frame.arg1 = base + 1120;
        frame.arg2 = 1;
        frame.arg3 = number == SYSCALL_epoll_pwait2 ? 0 : UINT64_MAX;
        frame.arg4 = base + 1024;
        frame.arg5 = sizeof(mask);
    }

    bool ok = expect(signals->BeginTemporaryMask(UINT64_MAX) == 0, "temporary mask installation");

    ok &= expect((task->signalMask & SIGNAL_UNBLOCKABLE) == 0, "temporary mask cannot block SIGKILL or SIGSTOP");

    signals->FinishTemporaryMask(false);

    ok &= expect(task->signalMask == blockedMask && !task->signalRestoreMask, "temporary mask explicit restoration");

    const uint64_t sent = syscall_call(SYSCALL_kill, process->id, 10);

    ok &= expect(sent == 0, "event blocked pending signal fixture");

    if (sent == 0)
    {
        ok &= expect(dispatcher_dispatch_syscall(&frame) == (uint64_t) -4, "temporary mask unblocks pending signal before parking");

        ok &= expect(task->signalMask == blockedMask && !task->signalRestoreMask, "kernel event caller restores mask after EINTR");

        if (arch_task_context_supported())
        {
            ok &= arch_selftest_event_signal(process, task, signals, frame, action, base, blockedMask);
        }
    }

    (void) signals->Action(10, &previous, nullptr);

    __atomic_store_n(&task->signalMask, originalMask, __ATOMIC_RELEASE);

    __atomic_store_n(&task->signalPending, originalPending, __ATOMIC_RELEASE);

    __atomic_store_n(&process->signalPending, processPending, __ATOMIC_RELEASE);

    task->signalSavedMask   = originalSaved;
    task->signalRestoreMask = originalRestore;
    task->signalDepth       = originalDepth;

    return ok;
}

static bool test_event_wait(process_t* process, uint64_t number)
{
    const bool     select  = number == SYSCALL_select || number == SYSCALL_pselect6;
    const bool     precise = number == SYSCALL_ppoll || number == SYSCALL_pselect6;
    const uint64_t base    = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    const uint64_t  input    = base + PAGE_SIZE - 4;
    const uint64_t  timeout  = base + 256;
    time_timespec_t duration = {};

    fixture_store(process, timeout, &duration, sizeof(duration));

    Dispatcher*   dispatcher  = static_cast<Dispatcher*>(platform.dispacher);
    EventManager* events      = dispatcher->GetLogicLayerCaps()->eventManager;
    uint64_t      oldSequence = events->Sequence();

    events->Notify();

    bool sequenceOk = expect(events->Wait(oldSequence) == 0, "event arriving before park prevents sleeping");

    uint64_t fd = syscall_call(SYSCALL_eventfd2, 0, 04000);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    auto call = [&]() -> uint64_t
    {
        return select ? syscall_call(number, fd + 1, input, base + 64, base + 128, timeout) : precise ? syscall_call(number, input, 1, timeout)
                                                                                                      : syscall_call(number, input, 1, 0);
    };
    event_pollfd_t pollfd  = {(int32_t) fd, 1, 0xffff};
    uint64_t       readSet = 1ULL << fd, writeSet = 0, exceptSet = 0;
    auto           prepare = [&]()
    {
        if (select)
        {
            fixture_store(process, input, &readSet, sizeof(readSet));

            fixture_store(process, base + 64, &writeSet, sizeof(writeSet));

            fixture_store(process, base + 128, &exceptSet, sizeof(exceptSet));
        }
        else
        {
            fixture_store(process, input, &pollfd, sizeof(pollfd));
        }
    };
    prepare();

    bool ok = sequenceOk && expect(call() == 0, "event readiness zero timeout on empty counter");

    if (!select)
    {
        event_pollfd_t actual;

        fixture_load(process, input, &actual, sizeof(actual));

        ok &= expect(actual.fd == pollfd.fd && actual.events == pollfd.events && actual.revents == 0, "poll writes only revents");

        pollfd.fd = -1;

        prepare();

        ok &= expect(call() == 0, "poll ignores negative descriptor");

        pollfd.fd = (int32_t) process->Files()->count - 1;

        prepare();

        ok &= expect(call() == 1, "poll invalid descriptor is reported as ready");

        fixture_load(process, input, &actual, sizeof(actual));

        ok &= expect(actual.revents == 0x20, "poll POLLNVAL without requested events");

        pollfd.fd = (int32_t) fd;
    }
    else
    {
        uint64_t invalid = 1ULL << (process->Files()->count - 1);

        fixture_store(process, input, &invalid, sizeof(invalid));

        ok &= expect(syscall_call(number, process->Files()->count, input, 0, 0, timeout) == LINUX_EBADF, "select invalid descriptor is EBADF");
    }

    uint64_t value = 9;

    fixture_store(process, base, &value, sizeof(value));

    syscall_call(SYSCALL_write, fd, base, 8);

    writeSet = select ? 1ULL << fd : 0;

    if (!select)
    {
        pollfd.events = 1 | 4;
    }

    prepare();

    ok &= expect(call() == (select ? 2ULL : 1ULL), "event counter read/write readiness and select ready-bit count");

    if (select)
    {
        ProcessManager*      processes = dispatcher->GetResourceLayerCaps()->processManager;
        process_file_limit_t limit     = {0, process->fileLimit.maximum}, original;

        processes->ChangeFileLimit(process, &limit, &original);

        prepare();

        ok &= expect(call() == 2, "select continues monitoring existing descriptors above a lowered soft limit");

        processes->ChangeFileLimit(process, &original, nullptr);
    }

    syscall_call(SYSCALL_read, fd, base, 8);

    writeSet      = 0;
    pollfd.events = 1;

    memcpy(fixture_bytes(process, base + 640), "/test.txt", sizeof("/test.txt"));

    uint64_t regular = syscall_call(SYSCALL_open, base + 640, 0);

    ok &= expect((int64_t) regular >= 0, "event regular-file fixture opens");

    if ((int64_t) regular >= 0)
    {
        syscall_call(SYSCALL_lseek, regular, 0, 2);

        uint64_t ready;

        if (select)
        {
            uint64_t bit = 1ULL << regular;

            fixture_store(process, input, &bit, sizeof(bit));

            fixture_store(process, base + 64, &bit, sizeof(bit));

            ready = syscall_call(number, regular + 1, input, base + 64, 0, timeout);
        }
        else
        {
            event_pollfd_t filePoll = {(int32_t) regular, 1 | 4, 0};

            fixture_store(process, input, &filePoll, sizeof(filePoll));

            ready = precise ? syscall_call(number, input, 1, timeout) : syscall_call(number, input, 1, 0);
        }

        ok &= expect(ready == (select ? 2ULL : 1ULL), "regular file remains readable/writable at EOF");

        syscall_call(SYSCALL_close, regular);
    }

    prepare();

    if (precise)
    {
        const uint64_t mask      = UINT64_MAX;
        const uint64_t bundle[2] = {base + 512, sizeof(mask)};

        fixture_store(process, base + 512, &mask, sizeof(mask));

        fixture_store(process, base + 544, bundle, sizeof(bundle));
    }

    ok &= event_with_peer(process, fd, false, [&]()
    {
        // Infinite waits need no clock and must be awakened by the producer.
        uint64_t result = select    ? syscall_call(number, fd + 1, input, 0, 0, 0, precise ? base + 544 : 0)
                          : precise ? syscall_call(number, input, 1, 0, base + 512, 8)
                                    : syscall_call(number, input, 1, UINT64_MAX);

        return expect(result == 1, "poll/select parked wait wakes on peer event");
    });
    syscall_call(SYSCALL_read, fd, base, 8);

    prepare();

    if (precise)
    {
        ok &= test_event_temporary_mask(process, number, base);

        uint64_t mask = 0;

        fixture_store(process, base + 512, &mask, sizeof(mask));

        uint64_t bundle[2] = {base + 512, 8};

        fixture_store(process, base + 544, bundle, sizeof(bundle));

        task_t*        task         = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->GetCurrentTask();
        const uint64_t originalMask = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);

        uint64_t masked = select ? syscall_call(number, 0, 0, 0, 0, timeout, base + 544) : syscall_call(number, 0, 0, timeout, base + 512, 8);

        ok &= expect(masked == 0 && task->signalMask == originalMask && !task->signalRestoreMask, "event temporary signal mask restored after timeout");

        mask = UINT64_MAX;

        fixture_store(process, base + 512, &mask, sizeof(mask));

        masked = select ? syscall_call(number, 0, 0, 0, 0, CANONICAL_KERNEL_BASE, base + 544) : syscall_call(number, 0, 0, CANONICAL_KERNEL_BASE, base + 512, 8);

        ok &= expect(masked == LINUX_EFAULT && task->signalMask == originalMask && !task->signalRestoreMask, "event temporary mask restored after invalid timeout");

        bundle[1] = 4;

        fixture_store(process, base + 544, bundle, sizeof(bundle));

        uint64_t badSize = select ? syscall_call(number, 0, 0, 0, 0, timeout, base + 544) : syscall_call(number, 0, 0, timeout, base + 512, 4);

        ok &= expect(badSize == LINUX_EINVAL, "event raw kernel signal-set size validation");

        duration = {-1, 0};

        fixture_store(process, timeout, &duration, sizeof(duration));

        ok &= expect(call() == LINUX_EINVAL, "event negative timespec");

        duration = {0, 1000000000};

        fixture_store(process, timeout, &duration, sizeof(duration));

        ok &= expect(call() == LINUX_EINVAL, "event invalid timespec nanoseconds");
    }

    duration = {};

    fixture_store(process, timeout, &duration, sizeof(duration));

    uint64_t invalidPointer = select    ? syscall_call(number, 1, CANONICAL_KERNEL_BASE, 0, 0, timeout)
                              : precise ? syscall_call(number, CANONICAL_KERNEL_BASE, 1, timeout)
                                        : syscall_call(number, CANONICAL_KERNEL_BASE, 1, 0);

    ok &= expect(invalidPointer == LINUX_EFAULT, "event invalid user input");

    if (select)
    {
        ok &= expect(syscall_call(number, UINT64_MAX, 0, 0, 0, timeout) == LINUX_EINVAL, "select negative nfds");
    }
    else
    {
        ok &= expect((precise ? syscall_call(number, 0, process->fileLimit.current + 1, timeout) : syscall_call(number, 0, process->fileLimit.current + 1, 0)) == LINUX_EINVAL,
            "poll count constrained by descriptor limit");
    }

    prepare();

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

    ok &= expect(call() == LINUX_EFAULT, "event readonly output boundary");

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);

    // Exercise a finite timeout, including raw timeval/timespec update.
    duration = {0, select && !precise ? 1000 : 1000000};

    fixture_store(process, timeout, &duration, sizeof(duration));

    time_timespec_t now;

    if (dispatcher->GetResourceLayerCaps()->timeManager->ReadClock(1, &now) == 0)
    {
        uint64_t result = select ? syscall_call(number, 0, 0, 0, 0, timeout) : precise ? syscall_call(number, 0, 0, timeout)
                                                                                       : syscall_call(number, 0, 0, 1);

        ok &= expect(result == 0, "event finite empty wait expires");

        if (select || precise)
        {
            fixture_load(process, timeout, &duration, sizeof(duration));

            ok &= expect(duration.seconds == 0 && duration.nanoseconds == 0, "event raw timeout updated to zero");
        }
    }
    else
    {
        kprintf("Arx kernel: event finite timeout selftest unavailable without monotonic clock\n");
    }

    ok &= cleanup(syscall_call(SYSCALL_close, fd) == 0, "event wait descriptor cleanup");

    return ok;
}

static bool test_event_epoll_partial_copy(process_t* process, uint64_t base)
{
    const uint64_t epfd = syscall_call(SYSCALL_epoll_create1, 0);

    const uint64_t first = syscall_call(SYSCALL_eventfd2, 1, 04000);

    const uint64_t second = syscall_call(SYSCALL_eventfd2, 1, 04000);

    bool ready = (int64_t) epfd >= 0 && (int64_t) first >= 0 && (int64_t) second >= 0;

    bool ok = expect(ready, "epoll partial-copy fixture");

    if (ready)
    {
        event_epoll_event_t event = {EVENT_POLL_IN | 0xc0000000U, 1};

        fixture_store(process, base + 2048, &event, sizeof(event));

        ready = syscall_call(SYSCALL_epoll_ctl, epfd, 1, first, base + 2048) == 0;

        event.data = 2;

        fixture_store(process, base + 2048, &event, sizeof(event));

        ready &= syscall_call(SYSCALL_epoll_ctl, epfd, 1, second, base + 2048) == 0;

        ok &= expect(ready, "epoll partial-copy watch registration");

        if (ready)
        {
            const uint64_t output = base + PAGE_SIZE - sizeof(event);

            syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

            ok &= expect(syscall_call(SYSCALL_epoll_wait, epfd, output, 2, 0) == 1, "epoll returns copied prefix before later output fault");

            event_epoll_event_t actual = {};

            fixture_load(process, output, &actual, sizeof(actual));

            ok &= expect(actual.events == EVENT_POLL_IN && actual.data == 1, "epoll first event copied before disarm");

            syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);

            ok &= expect(syscall_call(SYSCALL_epoll_wait, epfd, base + 2112, 2, 0) == 1, "epoll untransferred edge remains pending after partial output fault");

            fixture_load(process, base + 2112, &actual, sizeof(actual));

            ok &= expect(actual.data == 2 && syscall_call(SYSCALL_epoll_wait, epfd, base + 2112, 2, 0) == 0, "epoll disarms only successfully copied one-shot events");
        }
    }

    if ((int64_t) first >= 0)
    {
        syscall_call(SYSCALL_close, first);
    }

    if ((int64_t) second >= 0)
    {
        syscall_call(SYSCALL_close, second);
    }

    if ((int64_t) epfd >= 0)
    {
        syscall_call(SYSCALL_close, epfd);
    }

    return ok;
}

static bool test_event_nested_epoll(process_t* process, uint64_t base)
{
    uint64_t instances[6];
    bool     ready = true;

    for (size_t i = 0; i < 6; ++i)
    {
        instances[i] = syscall_call(SYSCALL_epoll_create1, 0);

        ready &= (int64_t) instances[i] >= 0;
    }

    const uint64_t counter = syscall_call(SYSCALL_eventfd2, 1, 04000);

    ready &= (int64_t) counter >= 0;

    bool ok = expect(ready, "nested epoll graph fixture");

    if (ready)
    {
        const uint64_t      input  = base + 2048;
        const uint64_t      output = base + 2112;
        event_epoll_event_t event  = {EVENT_POLL_IN, 17};

        fixture_store(process, input, &event, sizeof(event));

        for (size_t i = 0; i < 4; ++i)
        {
            ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[i], 1, instances[i + 1], input) == 0, "nested epoll accepts five instance levels");
        }

        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[4], 1, instances[5], input) == (uint64_t) -40, "nested epoll checks depth through existing ancestors");

        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[4], 1, instances[0], input) == (uint64_t) -40, "nested epoll rejects transitive cycle");

        event.events = EVENT_POLL_IN | 0x40000000U;

        fixture_store(process, input, &event, sizeof(event));

        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[4], 1, counter, input) == 0, "nested leaf one-shot registration");

        ok &= expect(syscall_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 1, "nested epoll propagates readiness");

        ok &= expect(syscall_call(SYSCALL_epoll_wait, instances[4], output, 1, 0) == 1 && syscall_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 0,
            "nested epoll reflects one-shot disarm");

        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[4], 3, counter, input) == 0, "nested leaf rearm");

        event.events = EVENT_POLL_IN | 0x80000000U;

        fixture_store(process, input, &event, sizeof(event));

        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[0], 3, instances[1], input) == 0, "nested outer edge registration");

        ok &= expect(syscall_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 1 && syscall_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 0,
            "nested outer edge delivered once");

        uint64_t value = 1;

        fixture_store(process, base, &value, sizeof(value));

        ok &= expect(syscall_call(SYSCALL_write, counter, base, 8) == 8 && syscall_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 1,
            "new leaf notification propagates through nested edge");

        ok &= expect(syscall_call(SYSCALL_read, counter, base, 8) == 8, "nested leaf drain");

        event.events = EVENT_POLL_IN | 0x40000000U;

        fixture_store(process, input, &event, sizeof(event));

        ok &= expect(syscall_call(SYSCALL_epoll_ctl, instances[4], 3, counter, input) == 0 && syscall_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 0,
            "empty nested graph is not readable");

        if (ok)
        {
            ok &= event_with_peer(process, counter, false,
                [&]()
            {
                return expect(syscall_call(SYSCALL_epoll_wait, instances[0], output, 1, UINT64_MAX) == 1, "parked nested edge wakes on peer write");
            });
        }

        ok &= expect(syscall_call(SYSCALL_close, instances[4]) == 0, "nested child final close");

        instances[4] = UINT64_MAX;

        ok &= expect(syscall_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 0, "nested child close detaches parent watch");
    }

    for (size_t i = 0; i < 6; ++i)
    {
        if ((int64_t) instances[i] >= 0)
        {
            syscall_call(SYSCALL_close, instances[i]);
        }
    }

    if ((int64_t) counter >= 0)
    {
        syscall_call(SYSCALL_close, counter);
    }

    return ok;
}

static bool test_event_epoll(process_t* process, uint64_t number)
{
    const uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    if (number == SYSCALL_epoll_ctl && (!test_event_nested_epoll(process, base) || !test_event_epoll_partial_copy(process, base) || !test_epoll_exclusive(process, base)))
    {
        return false;
    }

    const uint64_t input  = base + PAGE_SIZE - 4; // Packed event crosses a page.
    const uint64_t output = base + 128;
    const bool     legacy = number == SYSCALL_epoll_create;

    const uint64_t epfd = legacy ? syscall_call(number, 1) : syscall_call(SYSCALL_epoll_create1, 02000000);

    const uint64_t fd = syscall_call(SYSCALL_eventfd2, 0, 04000);

    if (!expect((int64_t) epfd >= 0 && (int64_t) fd >= 0, "epoll fixture descriptors"))
    {
        return false;
    }

    bool ok = expect(syscall_call(SYSCALL_fcntl, epfd, 1) == (legacy ? 0ULL : 1ULL), "epoll creation CLOEXEC flag");

    ok &= expect(syscall_call(SYSCALL_epoll_create, 0) == LINUX_EINVAL && syscall_call(SYSCALL_epoll_create, UINT64_MAX) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_epoll_create1, 04000) == LINUX_EINVAL,
        "epoll creation argument validation");

    event_epoll_event_t event = {EVENT_POLL_IN, 0xfedcba9876543210ULL}, actual = {};

    fixture_store(process, input, &event, sizeof(event));

    const uint64_t waitMask = UINT64_MAX;

    fixture_store(process, base + 800, &waitMask, sizeof(waitMask));

    auto control = [&](uint64_t operation, uint64_t target) -> uint64_t
    {
        return syscall_call(SYSCALL_epoll_ctl, epfd, operation, target, input);
    };
    auto wait = [&](uint64_t count, uint64_t milliseconds) -> uint64_t
    {
        if (number == SYSCALL_epoll_pwait2)
        {
            time_timespec_t duration = {(int64_t) (milliseconds / 1000), (int64_t) ((milliseconds % 1000) * 1000000)};

            fixture_store(process, base + 768, &duration, sizeof(duration));

            return syscall_call(number, epfd, output, count, milliseconds == UINT64_MAX ? 0 : base + 768, base + 800, 8);
        }

        const uint64_t waitNumber = number == SYSCALL_epoll_pwait ? number : SYSCALL_epoll_wait;

        return syscall_call(waitNumber, epfd, output, count, milliseconds, number == SYSCALL_epoll_pwait ? base + 800 : 0, 8);
    };
    ok &= expect(wait(1, 0) == 0, "empty epoll zero timeout");

    if (number == SYSCALL_epoll_pwait || number == SYSCALL_epoll_pwait2)
    {
        ok &= test_event_temporary_mask(process, number, base, epfd);

        task_t*         task         = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->GetCurrentTask();
        const uint64_t  originalMask = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);
        uint64_t        mask         = UINT64_MAX;
        time_timespec_t duration     = {};

        fixture_store(process, base + 768, &duration, sizeof(duration));

        fixture_store(process, base + 800, &mask, sizeof(mask));

        const uint64_t timeout = number == SYSCALL_epoll_pwait2 ? base + 768 : 0;

        ok &= expect(syscall_call(number, epfd, output, 1, timeout, 0, UINT64_MAX) == 0, "epoll null signal mask ignores size");

        ok &= expect(syscall_call(number, epfd, output, 1, timeout, base + 800, 8) == 0 && task->signalMask == originalMask && !task->signalRestoreMask,
            "epoll temporary mask restored after timeout");

        ok &= expect(syscall_call(number, epfd, output, 1, timeout, base + 800, 4) == LINUX_EINVAL, "epoll signal mask size validation");

        ok &= expect(syscall_call(number, epfd, output, 1, timeout, CANONICAL_KERNEL_BASE, 8) == LINUX_EFAULT, "epoll invalid signal mask buffer");

        ok &= expect(syscall_call(number, epfd, output, 0, timeout, base + 800, 8) == LINUX_EINVAL && task->signalMask == originalMask && !task->signalRestoreMask,
            "epoll temporary mask restored after invalid count");

        if (number == SYSCALL_epoll_pwait2)
        {
            duration = {-1, 0};

            fixture_store(process, base + 768, &duration, sizeof(duration));

            ok &= expect(syscall_call(number, epfd, output, 1, base + 768) == LINUX_EINVAL, "epoll_pwait2 negative seconds");

            duration = {0, 1000000000};

            fixture_store(process, base + 768, &duration, sizeof(duration));

            ok &= expect(syscall_call(number, epfd, output, 1, base + 768) == LINUX_EINVAL, "epoll_pwait2 invalid nanoseconds");

            ok &= expect(syscall_call(number, epfd, output, 1, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "epoll_pwait2 invalid timespec buffer");
        }
    }

    ok &= expect(control(3, fd) == LINUX_ENOENT && control(2, fd) == LINUX_ENOENT, "epoll missing MOD/DEL");

    ok &= expect(control(1, fd) == 0 && control(1, fd) == (uint64_t) -17, "epoll ADD and duplicate EEXIST");

    ok &= expect(wait(1, 0) == 0, "registered empty eventfd is not ready");

    ok &= expect(control(0, fd) == LINUX_EINVAL && control(1, UINT64_MAX) == LINUX_EBADF && control(1, epfd) == LINUX_EINVAL, "epoll operation/fd/self validation");

    ok &= expect(syscall_call(SYSCALL_epoll_ctl, epfd, 3, fd, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "epoll bad event input");

    const uint32_t unsupportedModes[] = {0x20000000U};

    for (uint32_t unsupported : unsupportedModes)
    {
        event.events = EVENT_POLL_IN | unsupported;

        fixture_store(process, input, &event, sizeof(event));

        ok &= expect(control(3, fd) == LINUX_EOPNOTSUPP, "epoll WAKEUP requires real suspend-blocking support");
    }

    event = {EVENT_POLL_IN, 0xfedcba9876543210ULL};

    fixture_store(process, input, &event, sizeof(event));

    const uint64_t other = syscall_call(SYSCALL_epoll_create1, 0);

    if ((int64_t) other >= 0)
    {
        ok &= expect(control(1, other) == 0, "nested epoll registration");

        ok &= expect(syscall_call(SYSCALL_epoll_ctl, other, 1, epfd, input) == (uint64_t) -40, "nested epoll rejects cycles");

        ok &= expect(syscall_call(SYSCALL_epoll_ctl, other, 1, fd, input) == 0 && syscall_call(SYSCALL_close, other) == 0, "one source can belong to independent epoll instances");
    }
    else
    {
        ok &= expect(false, "second epoll fixture");
    }

    memcpy(fixture_bytes(process, base + 640), "/test.txt", sizeof("/test.txt"));

    const uint64_t regular = syscall_call(SYSCALL_open, base + 640, 0);

    if ((int64_t) regular >= 0)
    {
        ok &= expect(control(1, regular) == (uint64_t) -1, "epoll rejects synchronous regular files with EPERM");

        ok &= expect(syscall_call(SYSCALL_epoll_wait, regular, output, 1, 0) == LINUX_EINVAL && syscall_call(SYSCALL_epoll_ctl, regular, 1, fd, input) == LINUX_EINVAL,
            "epoll operations reject non-epoll instance descriptors");

        syscall_call(SYSCALL_close, regular);
    }
    else
    {
        ok &= expect(false, "epoll regular file fixture");
    }

    uint64_t value = 1;

    fixture_store(process, base, &value, sizeof(value));

    ok &= expect(syscall_call(SYSCALL_write, fd, base, 8) == 8, "epoll eventfd producer");

    ok &= expect(wait(1, 0) == 1, "epoll ready event delivered");

    fixture_load(process, output, &actual, sizeof(actual));

    ok &= expect(actual.events == EVENT_POLL_IN && actual.data == event.data, "epoll packed mask and 64-bit opaque data");

    ok &= expect(wait(1, 0) == 1, "level-triggered readiness repeats without consumption");

    event_pollfd_t pollfd = {(int32_t) epfd, EVENT_POLL_IN, 0};

    fixture_store(process, output + 64, &pollfd, sizeof(pollfd));

    ok &= expect(syscall_call(SYSCALL_poll, output + 64, 1, 0) == 1, "epoll descriptor itself is pollable");

    ok &= expect(syscall_call(SYSCALL_epoll_wait, epfd, CANONICAL_KERNEL_BASE, 1, 0) == LINUX_EFAULT, "epoll rejects kernel output");

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_epoll_wait, epfd, input, 1, 0) == LINUX_EFAULT, "epoll rejects readonly output across page boundary");

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);

    ok &= expect(wait(1, 0) == 1, "epoll output fault preserves level readiness");

    const uint32_t modes[] = {0x80000000U, 0x40000000U, 0xc0000000U};

    for (uint32_t mode : modes)
    {
        event = {EVENT_POLL_IN | mode, mode};

        fixture_store(process, input, &event, sizeof(event));

        ok &= expect(control(3, fd) == 0, "epoll ET/ONESHOT MOD rearm");

        syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

        ok &= expect(syscall_call(SYSCALL_epoll_wait, epfd, input, 1, 0) == LINUX_EFAULT, "epoll output fault preserves pending edge and one-shot arm");

        syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);

        ok &= expect(wait(1, 0) == 1 && wait(1, 0) == 0, "epoll edge/one-shot delivers once");

        fixture_load(process, output, &actual, sizeof(actual));

        ok &= expect(actual.events == EVENT_POLL_IN && actual.data == mode, "epoll private flags excluded from returned mask");

        value = 1;

        fixture_store(process, base, &value, sizeof(value));

        ok &= expect(syscall_call(SYSCALL_write, fd, base, 8) == 8, "epoll fresh source notification");

        const uint64_t expected = (mode & 0x40000000U) != 0 ? 0 : 1;

        ok &= expect(wait(1, 0) == expected && wait(1, 0) == 0, "new writes notify edges but do not rearm one-shot");

        ok &= expect(control(3, fd) == 0 && wait(1, 0) == 1 && wait(1, 0) == 0, "epoll MOD rechecks existing readiness");
    }

    event = {EVENT_POLL_OUT, 42};

    fixture_store(process, input, &event, sizeof(event));

    ok &= expect(control(3, fd) == 0 && wait(1, 0) == 1, "epoll MOD updates interest");

    fixture_load(process, output, &actual, sizeof(actual));

    ok &= expect(actual.events == EVENT_POLL_OUT && actual.data == 42, "epoll MOD updates opaque data");

    ok &= expect(syscall_call(SYSCALL_epoll_ctl, epfd, 2, fd, CANONICAL_KERNEL_BASE) == 0 && wait(1, 0) == 0, "epoll DEL ignores event pointer and removes registration");

    event = {EVENT_POLL_IN, 1};

    fixture_store(process, input, &event, sizeof(event));

    ok &= expect(control(1, fd) == 0, "epoll re-add after DEL");

    const uint64_t duplicate = syscall_call(SYSCALL_dup, fd);

    if ((int64_t) duplicate >= 0)
    {
        event.data = 2;

        fixture_store(process, input, &event, sizeof(event));

        ok &= expect(control(1, duplicate) == 0, "epoll keys include fd and open file description");

        event_epoll_event_t first = {}, second = {};

        ok &= expect(wait(1, 0) == 1, "epoll first bounded batch");

        fixture_load(process, output, &first, sizeof(first));

        ok &= expect(wait(1, 0) == 1, "epoll second bounded batch");

        fixture_load(process, output, &second, sizeof(second));

        ok &= expect(first.data != second.data, "epoll round-robin fairness");

        ok &= expect(syscall_call(SYSCALL_close, fd) == 0 && wait(2, 0) == 2, "epoll original registration survives close while dup remains");

        const uint64_t replacement = syscall_call(SYSCALL_eventfd2, 0, 04000);

        ok &= expect(replacement == fd && control(2, replacement) == LINUX_ENOENT, "epoll fd reuse cannot address old open file registration");

        if ((int64_t) replacement >= 0)
        {
            syscall_call(SYSCALL_close, replacement);
        }

        ok &= expect(syscall_call(SYSCALL_close, duplicate) == 0 && wait(2, 0) == 0, "epoll final source close removes all weak registrations");
    }
    else
    {
        ok &= expect(false, "epoll dup fixture");

        syscall_call(SYSCALL_close, fd);
    }

    const uint64_t source = syscall_call(SYSCALL_eventfd2, 0, 04000);

    if ((int64_t) source >= 0)
    {
        fixture_store(process, input, &event, sizeof(event));

        const bool registered = control(1, source) == 0;

        ok &= expect(registered, "epoll blocking source registration");

        if (registered)
        {
            ok &= event_with_peer(process, source, false, [&]()
            {
                return expect(wait(1, UINT64_MAX) == 1, "parked epoll wait wakes on real peer write");
            });
        }

        syscall_call(SYSCALL_read, source, base, 8);

        time_timespec_t now;
        Dispatcher*     dispatcher = static_cast<Dispatcher*>(platform.dispacher);
        TimeManager*    clock      = dispatcher->GetResourceLayerCaps()->timeManager;

        if (clock != nullptr && clock->ReadClock(1, &now) == 0)
        {
            ok &= expect(wait(1, 1) == 0, "epoll finite timeout expires");
        }
        else
        {
            kprintf("Arx kernel: epoll finite timeout selftest unavailable without monotonic clock\n");
        }

        const uint64_t alias = syscall_call(SYSCALL_dup, epfd);

        if ((int64_t) alias >= 0)
        {
            ok &= expect(syscall_call(SYSCALL_close, epfd) == 0 && syscall_call(SYSCALL_epoll_wait, alias, output, 1, 0) == 0, "epoll duplicated instance survives original close");

            syscall_call(SYSCALL_close, alias); // Remove watches while source is live.
        }
        else
        {
            ok &= expect(false, "epoll instance dup fixture");

            syscall_call(SYSCALL_close, epfd);
        }

        ok &= cleanup(syscall_call(SYSCALL_close, source) == 0, "epoll instance/source cleanup");
    }
    else
    {
        ok &= expect(false, "epoll blocking source fixture");

        syscall_call(SYSCALL_close, epfd);
    }

    ok &= expect(syscall_call(SYSCALL_epoll_wait, UINT64_MAX, output, 1, 0) == LINUX_EBADF && syscall_call(SYSCALL_epoll_wait, UINT64_MAX, output, 0, 0) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_epoll_wait, UINT64_MAX, output, UINT64_MAX, 0) == LINUX_EINVAL,
        "epoll wait fd/maxevents validation");

    Dispatcher*          dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ProcessManager*      processes  = dispatcher->GetResourceLayerCaps()->processManager;
    process_file_limit_t old        = {}, limit = {0, process->fileLimit.maximum};

    processes->ChangeFileLimit(process, &limit, &old);

    ok &= expect(syscall_call(SYSCALL_epoll_create1, 0) == LINUX_EMFILE, "epoll creation descriptor limit rollback");

    processes->ChangeFileLimit(process, &old, nullptr);

    // Exercise generic process teardown with live registrations. The isolated
    // fixture closes these descriptors through ProcessManager::FreeProcess.
    const uint64_t teardownInstance = syscall_call(SYSCALL_epoll_create1, 0);

    const uint64_t teardownSource = syscall_call(SYSCALL_eventfd2, 0, 04000);

    ok &= expect((int64_t) teardownInstance >= 0 && (int64_t) teardownSource >= 0 && syscall_call(SYSCALL_epoll_ctl, teardownInstance, 1, teardownSource, input) == 0,
        "epoll process teardown fixture retains live watch");

    return ok;
}

template <uint64_t Number>
static bool validate_event(const syscall_case_t*)
{
    return with_process([](process_t* process)
    {
        if (Number == SYSCALL_epoll_create || Number == SYSCALL_epoll_create1 || Number == SYSCALL_epoll_ctl || Number == SYSCALL_epoll_wait || Number == SYSCALL_epoll_pwait ||
            Number == SYSCALL_epoll_pwait2)
        {
            return test_event_epoll(process, Number);
        }

        if (Number == SYSCALL_poll)
        {
            const uint64_t base = fixture_map(2);
            if ((int64_t) base < 0 || !test_readiness_reuse(process, base) || !test_readiness_producers(process, base) || !test_readiness_interruption(process, base))
            {
                return false;
            }
        }

        return Number == SYSCALL_eventfd || Number == SYSCALL_eventfd2 ? test_event_counter(process, Number) : test_event_wait(process, Number);
    });
}

static bool test_inotify_unlinked(process_t* process, uint64_t base)
{
    auto* dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto* resources  = dispatcher->GetResourceLayerCaps();
    auto* backend    = new SocketFileSystemFixture();

    if (backend == nullptr)
    {
        return false;
    }

    bool ok = true;
    {
        VirtualFileSystem vfs(resources);
        EventManager      events(resources, dispatcher->GetLogicLayerCaps()->scheduler, &vfs);
        vfs.SetEventManager(&events);
        ok &= expect(vfs.SetRootMount(&backend->mount), "unlink-filter writable filesystem fixture");
        inode_t* inode   = nullptr;
        inode_t* rebound = nullptr;
        ok &= expect(vfs.SocketNode("/entry", true, &inode) == 0, "unlink-filter live inode");
        const uint64_t normal    = events.CreateInotify(04000);
        const uint64_t exclusive = events.CreateInotify(04000);
        const char     root[]    = "/", entry[] = "/entry";
        fixture_store(process, base + 128, root, sizeof(root));
        fixture_store(process, base + 256, entry, sizeof(entry));

        if ((int64_t) normal >= 0 && (int64_t) exclusive >= 0 && inode != nullptr)
        {
            const uint32_t mask           = 1 | 0x200 | 0x40 | 0x80;
            const uint64_t directoryWatch = events.AddWatch(exclusive, base + 128, mask | 0x04000000);
            const uint64_t directWatch    = events.AddWatch(exclusive, base + 256, 1);
            ok &= expect((int64_t) events.AddWatch(normal, base + 128, mask) > 0 && (int64_t) directoryWatch > 0 && (int64_t) directWatch > 0,
                "IN_EXCL_UNLINK is stored on a real directory watch");

            file_t original = {}, alias = {}, replacement = {};
            original.inode  = alias.inode = inode;
            ok &= expect(vfs.ResolvePath({}, "/entry", &original.path) && vfs.Link({}, "/entry", {}, "/alias", false) == 0 && vfs.ResolvePath({}, "/alias", &alias.path),
                "hard-linked child entry identities");
            ok &= expect(vfs.Rename({}, "/entry", {}, "/alias") == 0 && inode->linkCount == 2, "same-inode hard-link rename preserves both entry identities");
            ok &= expect(vfs.Unlink({}, "/entry", false) == 0 && inode->linkCount == 1, "unlink only one directory entry of a live inode");
            ok &= expect(syscall_call(SYSCALL_read, normal, base + 1024, 512) == 32 && syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 32,
                "IN_EXCL_UNLINK retains the deletion event itself");

            vfs.NotifyFile(&original, 1);
            ok &= expect(syscall_call(SYSCALL_read, normal, base + 1024, 512) == 32 && syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 16,
                "post-unlink child event is suppressed only for the directory flag, not a direct inode watch");

            uint32_t record[4] = {};
            fixture_load(process, base + 1536, record, sizeof(record));
            ok &= expect(record[0] == directWatch && record[1] == 1, "direct inode watch survives child-entry unlink");

            vfs.NotifyFile(&alias, 1);
            ok &= expect(syscall_call(SYSCALL_read, normal, base + 1024, 512) == 32 && syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 48,
                "still-linked alias is not excluded by the other entry's unlink");

            ok &= expect(vfs.SocketNode("/entry", true, &rebound) == 0 && rebound != inode && vfs.ResolvePath({}, "/entry", &replacement.path), "unlink then rebind gets a fresh entry identity");
            replacement.inode = rebound;
            vfs.NotifyFile(&replacement, 1);
            ok &= expect(syscall_call(SYSCALL_read, normal, base + 1024, 512) == 32 && syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 32,
                "new entry with a reused name is not suppressed");

            ok &= expect(vfs.Rename({}, "/alias", {}, "/renamed") == 0, "rename an entry with an open description");
            ok &= expect(syscall_call(SYSCALL_read, normal, base + 1024, 512) == 64 && syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 64,
                "directory move events retain their cookies and names");
            vfs.NotifyFile(&alias, 1);
            ok &= expect(syscall_call(SYSCALL_read, normal, base + 1024, 512) == 32 && syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 48,
                "open description follows renamed entry rather than appearing unlinked");

            char name[16] = {};
            fixture_load(process, base + 1536 + 16, name, sizeof(name));
            ok &= expect(memcmp(name, "renamed", 8) == 0, "post-rename file events report the current entry name");

            ok &= expect(vfs.Rename({}, "/renamed", {}, "/again") == 0 && syscall_call(SYSCALL_read, normal, base + 1024, 512) == 64 &&
                             syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 64, "successive renames retain open-entry identity");
            vfs.NotifyFile(&alias, 1);
            ok &= expect(syscall_call(SYSCALL_read, normal, base + 1024, 512) == 32 && syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 48,
                "open description follows multiple rename records");
            fixture_load(process, base + 1536 + 16, name, sizeof(name));
            ok &= expect(memcmp(name, "again", 6) == 0, "latest rename supplies the child event name");

            ok &= expect(vfs.Unlink({}, "/again", false) == 0 && syscall_call(SYSCALL_read, normal, base + 1024, 512) == 32 &&
                             syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 32, "unlink the current renamed alias");
            vfs.NotifyFile(&alias, 1);
            ok &= expect(syscall_call(SYSCALL_read, normal, base + 1024, 512) == 32 && syscall_call(SYSCALL_read, exclusive, base + 1536, 512) == 16,
                "unlink after multiple renames suppresses only subsequent directory-child events");
        }
        else
        {
            ok &= expect(false, "unlink-filter notification descriptor allocation");
        }

        if ((int64_t) normal >= 0)
        {
            syscall_call(SYSCALL_close, normal);
        }

        if ((int64_t) exclusive >= 0)
        {
            syscall_call(SYSCALL_close, exclusive);
        }

        if (rebound != nullptr)
        {
            vfs.Unlink({}, "/entry", false);
            vfs.ReleaseSocketNode(rebound);
        }

        if (inode != nullptr)
        {
            vfs.ReleaseSocketNode(inode);
        }
    }

    delete backend;
    return ok;
}

static bool test_inotify_framework(process_t* process, uint64_t base, uint64_t number)
{
    const uint64_t fd = number == SYSCALL_inotify_init ? syscall_call(number) : syscall_call(SYSCALL_inotify_init1, 04000 | 02000000);

    if (!expect((int64_t) fd >= 0, "inotify creates a readable notification descriptor"))
    {
        return false;
    }

    bool ok = expect(syscall_call(SYSCALL_fcntl, fd, 1) == (uint64_t) (number != SYSCALL_inotify_init), "inotify CLOEXEC creation flag");

    syscall_call(SYSCALL_fcntl, fd, 4, 04000);

    const char path[] = "/test.txt", root[] = "/";

    fixture_store(process, base + 128, path, sizeof(path));

    fixture_store(process, base + 256, root, sizeof(root));

    ok &= expect(syscall_call(SYSCALL_read, fd, base + 1024, 512) == (uint64_t) -11 && syscall_call(SYSCALL_inotify_init1, 1) == LINUX_EINVAL,
        "inotify empty nonblocking read and creation flags");

    const uint64_t watch = syscall_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x31);

    ok &= expect((int64_t) watch > 0 && syscall_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x10000020) == (uint64_t) -17, "inotify watch allocation and MASK_CREATE collision");

    ok &= expect(syscall_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x01000020) == LINUX_ENOTDIR &&
                     syscall_call(SYSCALL_inotify_add_watch, fd, CANONICAL_KERNEL_BASE, 0x20) == LINUX_EFAULT,
        "inotify target flags and user path validation");

    const uint64_t opened = syscall_call(SYSCALL_open, base + 128, 0);

    ok &= expect((int64_t) opened >= 0 && syscall_call(SYSCALL_read, opened, base + 512, 4) == 4, "watched VFS open and read");

    if ((int64_t) opened >= 0)
    {
        syscall_call(SYSCALL_close, opened);
    }

    ok &= expect(syscall_call(SYSCALL_ioctl, fd, 0x541b, base + 64) == 0, "inotify FIONREAD reports complete record bytes");

    uint32_t queued = 0;

    fixture_load(process, base + 64, &queued, sizeof(queued));

    ok &= expect(queued == 48 && syscall_call(SYSCALL_read, fd, base + 1024, 15) == LINUX_EINVAL && syscall_call(SYSCALL_read, fd, CANONICAL_KERNEL_BASE, 512) == LINUX_EFAULT,
        "inotify short/faulting buffers preserve queued events");

    ok &= expect(syscall_call(SYSCALL_read, fd, base + 1024, 512) == 48, "inotify batches open/access/close records");

    uint32_t events[12] = {};

    fixture_load(process, base + 1024, events, sizeof(events));

    ok &= expect(events[0] == watch && events[1] == 0x20 && events[3] == 0 && events[4] == watch && events[5] == 1 && events[8] == watch && events[9] == 0x10,
        "Linux inotify_event 16-byte headers and event ordering");

    ok &= expect(syscall_call(SYSCALL_inotify_rm_watch, fd, watch) == 0 && syscall_call(SYSCALL_inotify_rm_watch, fd, watch) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_read, fd, base + 1024, 512) == 16,
        "inotify removal queues one IN_IGNORED record");

    fixture_load(process, base + 1024, events, 16);

    ok &= expect(events[1] == 0x8000, "inotify ignored event wire mask");

    const uint64_t directoryWatch = syscall_call(SYSCALL_inotify_add_watch, fd, base + 256, 0x20 | 0x80000000U);

    const uint64_t another = syscall_call(SYSCALL_open, base + 128, 0);

    if ((int64_t) another >= 0)
    {
        syscall_call(SYSCALL_close, another);
    }

    ok &= expect((int64_t) directoryWatch > 0 && syscall_call(SYSCALL_read, fd, base + 1024, 512) == 48, "directory oneshot produces padded child event and ignored record");

    uint8_t record[48] = {};

    fixture_load(process, base + 1024, record, sizeof(record));

    uint32_t nameLength = 0;

    memcpy(&nameLength, record + 12, 4);

    ok &= expect(nameLength == 16 && memcmp(record + 16, "test.txt", 9) == 0 && syscall_call(SYSCALL_inotify_rm_watch, fd, directoryWatch) == LINUX_EINVAL,
        "inotify directory name padding and oneshot teardown");

    const uint64_t blockingWatch = syscall_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x20);

    syscall_call(SYSCALL_fcntl, fd, 4, 0);

    syscall_peer_context_t peer = {nullptr, SYSCALL_open, {base + 128, 0}, UINT64_MAX};

    ok &= with_syscall_peer(process, &peer, [&]()
    {
        return expect(syscall_call(SYSCALL_read, fd, base + 1024, 512) == 16 && (int64_t) peer.result >= 0, "blocking inotify read wakes on real peer VFS open");
    });

    if ((int64_t) peer.result >= 0)
    {
        syscall_call(SYSCALL_close, peer.result);
    }

    syscall_call(SYSCALL_inotify_rm_watch, fd, blockingWatch);

    ok &= expect(syscall_call(SYSCALL_read, fd, base + 1024, 512) == 16, "drain ignored event before queue overflow test");

    const uint64_t overflowWatch = syscall_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x30);

    for (size_t i = 0; i < 40; ++i)
    {
        const uint64_t item = syscall_call(SYSCALL_open, base + 128, 0);

        if ((int64_t) item >= 0)
        {
            syscall_call(SYSCALL_close, item);
        }
    }

    ok &= expect(syscall_call(SYSCALL_read, fd, base + 2048, 2048) == 1024, "inotify queue remains bounded under overflow");

    uint32_t overflow[4] = {};

    fixture_load(process, base + 2048 + 63 * 16, overflow, sizeof(overflow));

    ok &= expect(overflow[0] == UINT32_MAX && overflow[1] == 0x4000 && overflow[3] == 0, "inotify queue overflow ABI marker");

    syscall_call(SYSCALL_inotify_rm_watch, fd, overflowWatch);

    ok &= expect(syscall_call(SYSCALL_inotify_add_watch, UINT32_MAX, CANONICAL_KERNEL_BASE, 1) == LINUX_EBADF &&
                     syscall_call(SYSCALL_inotify_add_watch, fd, base + 128, 0) == LINUX_EINVAL,
        "inotify descriptor and empty event mask validation");

    syscall_call(SYSCALL_close, fd);

    return ok;
}

static bool test_signalfd_framework(process_t* process, uint64_t base, uint64_t number)
{
    const uint64_t mask = (1ULL << 9) | (1ULL << 11) | SIGNAL_UNBLOCKABLE;

    fixture_store(process, base + 128, &mask, sizeof(mask));

    bool ok = expect(syscall_call(SYSCALL_rt_sigprocmask, 0, base + 128, 0, 8) == 0, "block signals before receiving through signalfd");

    const uint64_t fd = syscall_call(number, UINT32_MAX, base + 128, 8, number == SYSCALL_signalfd4 ? 04000 | 02000000 : 0);

    if (!expect((int64_t) fd >= 0, "signalfd creation"))
    {
        return false;
    }

    ok &= expect(syscall_call(SYSCALL_fcntl, fd, 1) == (uint64_t) (number == SYSCALL_signalfd4), "signalfd creation CLOEXEC");

    syscall_call(SYSCALL_fcntl, fd, 4, 04000);

    ok &= expect(syscall_call(SYSCALL_read, fd, base + 512, 128) == (uint64_t) -11 && syscall_call(SYSCALL_read, fd, base + 512, 127) == LINUX_EINVAL,
        "signalfd no pending record and exact record-size minimum");

    const uint64_t tid = syscall_call(SYSCALL_gettid);

    ok &= expect(syscall_call(SYSCALL_kill, process->id, 10) == 0 && syscall_call(SYSCALL_tgkill, process->id, tid, 12) == 0, "queue process and thread directed signalfd signals");

    event_pollfd_t poll = {(int32_t) fd, 1, 0};

    fixture_store(process, base + 256, &poll, sizeof(poll));

    ok &= expect(syscall_call(SYSCALL_poll, base + 256, 1, 0) == 1 && syscall_call(SYSCALL_read, fd, CANONICAL_KERNEL_BASE, 256) == LINUX_EFAULT,
        "signalfd readiness and fault-safe signal retention");

    file_iovec_t vectors[2] = {{base + 512, 64}, {base + 1024, 192}};

    fixture_store(process, base + 384, vectors, sizeof(vectors));

    ok &= expect(syscall_call(SYSCALL_readv, fd, base + 384, 2) == 256, "signalfd readv scatters two full records");

    uint8_t first[128] = {}, second[128] = {};

    fixture_load(process, base + 512, first, 64);

    fixture_load(process, base + 1024, first + 64, 64);

    fixture_load(process, base + 1088, second, 128);

    uint32_t info[5] = {};

    memcpy(info, first, sizeof(info));

    ok &= expect(info[0] == 10 && info[2] == 0 && info[3] == process->id && info[4] == process->credentials.user.real, "signalfd process sender info ABI");

    memcpy(info, second, sizeof(info));

    ok &= expect(info[0] == 12 && (int32_t) info[2] == -6 && info[3] == process->id, "signalfd thread sender info ABI");

    bool zero = true;

    for (size_t i = 20; i < 128; ++i)
    {
        zero &= first[i] == 0 && second[i] == 0;
    }

    ok &= expect(zero && syscall_call(SYSCALL_read, fd, base + 512, 128) == (uint64_t) -11, "signalfd zeroes reserved fields and consumes signals exactly once");

    ok &= expect(syscall_call(SYSCALL_signalfd4, fd, base + 128, 8, 02000000) == fd && syscall_call(SYSCALL_fcntl, fd, 3) == 04000 &&
                     syscall_call(SYSCALL_fcntl, fd, 1) == (uint64_t) (number == SYSCALL_signalfd4),
        "updating signalfd mask leaves existing descriptor flags unchanged");

    syscall_call(SYSCALL_fcntl, fd, 4, 0);

    syscall_peer_context_t peer = {nullptr, SYSCALL_kill, {process->id, 10}, UINT64_MAX};

    ok &= with_syscall_peer(
        process, &peer, [&]()
    {
        return expect(syscall_call(SYSCALL_read, fd, base + 512, 128) == 128 && peer.result == 0, "blocked signalfd wakes for a masked peer signal");
    });
    const uint64_t alarmMask = mask | (1ULL << 13);

    fixture_store(process, base + 128, &alarmMask, sizeof(alarmMask));

    time_itimerval_t timer = {{0, 0}, {0, 1000}};

    fixture_store(process, base + 2048, &timer, sizeof(timer));

    ok &= expect(syscall_call(SYSCALL_rt_sigprocmask, 0, base + 128, 0, 8) == 0 && syscall_call(SYSCALL_signalfd4, fd, base + 128, 8, 0) == fd &&
                     syscall_call(SYSCALL_setitimer, 0, base + 2048, 0) == 0 && syscall_call(SYSCALL_read, fd, base + 512, 128) == 128,
        "signalfd wakes for a blocked real-timer signal");

    fixture_load(process, base + 512, info, sizeof(info));

    ok &= expect(info[0] == 14 && info[2] == 128, "signalfd timer SI_KERNEL record");

    const uint64_t rtMask = (1ULL << 31) | (1ULL << 33) | (1ULL << 63);
    fixture_store(process, base + 128, &rtMask, sizeof(rtMask));
    ok &= expect(syscall_call(SYSCALL_rt_sigprocmask, 0, base + 128, 0, 8) == 0 && syscall_call(SYSCALL_signalfd4, fd, base + 128, 8, 0) == fd, "signalfd accepts RT mask including signals 32 and 64");
    ok &= expect(syscall_call(SYSCALL_kill, process->id, 64) == 0 && syscall_call(SYSCALL_kill, process->id, 32) == 0 && syscall_call(SYSCALL_kill, process->id, 32) == 0, "signalfd queues repeated process RT signals");
    ok &= expect(syscall_call(SYSCALL_tgkill, process->id, tid, 34) == 0 && syscall_call(SYSCALL_tgkill, process->id, tid, 34) == 0, "signalfd queues repeated thread RT signals");
    ok &= expect(syscall_call(SYSCALL_read, fd, CANONICAL_KERNEL_BASE, 128) == LINUX_EFAULT, "RT signalfd fault retains queued records");
    const uint32_t expectedSignals[] = {34, 34, 32, 32, 64};

    for (uint32_t expectedSignal : expectedSignals)
    {
        ok &= expect(syscall_call(SYSCALL_read, fd, base + 512, 128) == 128, "RT signalfd returns one complete queued record");
        fixture_load(process, base + 512, info, sizeof(info));
        ok &= expect(info[0] == expectedSignal && static_cast<int32_t>(info[2]) == (expectedSignal == 34 ? -6 : 0) && info[3] == process->id, "RT signalfd target precedence, number ordering and sender metadata");
    }

    uint8_t        queuedInfo[128] = {};
    const int32_t  queueCode       = -1;
    const int32_t  queuePid        = process->id;
    const uint32_t queueUid        = process->credentials.user.real;
    memcpy(queuedInfo + 8, &queueCode, 4);
    memcpy(queuedInfo + 16, &queuePid, 4);
    memcpy(queuedInfo + 20, &queueUid, 4);
    const uint64_t payloads[] = {0x1122334455667788ULL, 0x8877665544332211ULL};

    for (uint64_t payload : payloads)
    {
        memcpy(queuedInfo + 24, &payload, 8);
        fixture_store(process, base + 2048, queuedInfo, sizeof(queuedInfo));
        ok &= expect(syscall_call(SYSCALL_rt_tgsigqueueinfo, process->id, tid, 34, base + 2048) == 0, "queue thread signal with full-width payload");
    }

    for (uint64_t payload : payloads)
    {
        ok &= expect(syscall_call(SYSCALL_read, fd, base + 512, 128) == 128, "consume queued payload record");
        uint64_t actual  = 0;
        int32_t  integer = 0;
        fixture_load(process, base + 512 + 48, &actual, 8);
        fixture_load(process, base + 512 + 44, &integer, 4);
        ok &= expect(actual == payload && integer == static_cast<int32_t>(payload), "signalfd preserves pointer and integer payload in FIFO order");
    }

    ok &= expect(syscall_call(SYSCALL_rt_sigqueueinfo, process->id, 34, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "queued signal validates complete user siginfo");
    ok &= expect(syscall_call(SYSCALL_fcntl, fd, 4, 04000) == 0 && syscall_call(SYSCALL_read, fd, base + 512, 128) == static_cast<uint64_t>(-11), "RT signalfd drains repeated records exactly once");

    for (size_t index = 0; index < signal_queue_t::CAPACITY; ++index)
    {
        ok &= expect(syscall_call(SYSCALL_kill, process->id, 32) == 0, "RT send fills bounded queue");
    }

    ok &= expect(syscall_call(SYSCALL_kill, process->id, 32) == static_cast<uint64_t>(-11), "RT syscall queue exhaustion returns EAGAIN");

    for (size_t index = 0; index < signal_queue_t::CAPACITY; ++index)
    {
        ok &= expect(syscall_call(SYSCALL_read, fd, base + 512, 128) == 128, "exhaustion retains all previously queued records");
    }

    ok &= expect(syscall_call(SYSCALL_read, fd, base + 512, 128) == static_cast<uint64_t>(-11), "queue summary clears only after final repeated record");

    syscall_call(SYSCALL_fcntl, fd, 4, 0);
    syscall_peer_context_t rtPeer = {nullptr, SYSCALL_kill, {process->id, 34}, UINT64_MAX};
    ok &= with_syscall_peer(process, &rtPeer, [&]()
    {
        return expect(syscall_call(SYSCALL_read, fd, base + 512, 128) == 128 && rtPeer.result == 0, "blocked signalfd wakes for a masked real-time peer signal");
    });

    ok &= expect(syscall_call(SYSCALL_signalfd4, fd, base + 128, 4, 0) == LINUX_EINVAL && syscall_call(SYSCALL_signalfd4, fd, CANONICAL_KERNEL_BASE, 8, 0) == LINUX_EFAULT &&
                     syscall_call(SYSCALL_signalfd4, UINT32_MAX, base + 128, 8, 1) == LINUX_EINVAL,
        "signalfd sigset size, pointer and flag validation");

    syscall_call(SYSCALL_close, fd);

    return ok;
}

template <uint64_t Number>
static bool validate_notification(const syscall_case_t*)
{
    return with_process([](process_t* process)
    {
        auto           task    = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->GetCurrentTask();
        const uint64_t oldMask = task->signalMask, oldPending = task->signalPending;
        const uint64_t base    = fixture_map(4);
        bool           ok   = (int64_t) base >= 0 &&
                              (Number == SYSCALL_signalfd || Number == SYSCALL_signalfd4 ? test_signalfd_framework(process, base, Number) : test_inotify_framework(process, base, Number));

        task->signalMask    = oldMask;
        task->signalPending = oldPending;

        if (Number == SYSCALL_inotify_add_watch && (int64_t) base >= 0)
        {
            ok &= test_inotify_unlinked(process, base);
        }

        ok &= expect(descriptor_count(process) == 0, "notification fixture releases every descriptor");

        return ok;
    });
}

void test_event_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<EventRequestManager> tests[] = {
        {"HandlePollRequest", &EventRequestManager::HandlePollRequest, SYSCALL_poll, validate_event<SYSCALL_poll>},
        {"HandlePpollRequest", &EventRequestManager::HandlePpollRequest, SYSCALL_ppoll, validate_event<SYSCALL_ppoll>},
        {"HandleSelectRequest", &EventRequestManager::HandleSelectRequest, SYSCALL_select, validate_event<SYSCALL_select>},
        {"HandlePselect6Request", &EventRequestManager::HandlePselect6Request, SYSCALL_pselect6, validate_event<SYSCALL_pselect6>},
        {"HandleEpoll_createRequest", &EventRequestManager::HandleEpoll_createRequest, SYSCALL_epoll_create, validate_event<SYSCALL_epoll_create>},
        {"HandleEpoll_create1Request", &EventRequestManager::HandleEpoll_create1Request, SYSCALL_epoll_create1, validate_event<SYSCALL_epoll_create1>},
        {"HandleEpoll_ctlRequest", &EventRequestManager::HandleEpoll_ctlRequest, SYSCALL_epoll_ctl, validate_event<SYSCALL_epoll_ctl>},
        {"HandleEpoll_waitRequest", &EventRequestManager::HandleEpoll_waitRequest, SYSCALL_epoll_wait, validate_event<SYSCALL_epoll_wait>},
        {"HandleEpoll_pwaitRequest", &EventRequestManager::HandleEpoll_pwaitRequest, SYSCALL_epoll_pwait, validate_event<SYSCALL_epoll_pwait>},
        {"HandleEpoll_pwait2Request", &EventRequestManager::HandleEpoll_pwait2Request, SYSCALL_epoll_pwait2, validate_event<SYSCALL_epoll_pwait2>},
        {"HandleInotify_initRequest", &EventRequestManager::HandleInotify_initRequest, SYSCALL_inotify_init, validate_notification<SYSCALL_inotify_init>},
        {"HandleInotify_init1Request", &EventRequestManager::HandleInotify_init1Request, SYSCALL_inotify_init1, validate_notification<SYSCALL_inotify_init1>},
        {"HandleInotify_add_watchRequest", &EventRequestManager::HandleInotify_add_watchRequest, SYSCALL_inotify_add_watch, validate_notification<SYSCALL_inotify_add_watch>},
        {"HandleInotify_rm_watchRequest", &EventRequestManager::HandleInotify_rm_watchRequest, SYSCALL_inotify_rm_watch, validate_notification<SYSCALL_inotify_rm_watch>},
        {"HandleEventfdRequest", &EventRequestManager::HandleEventfdRequest, SYSCALL_eventfd, validate_event<SYSCALL_eventfd>},
        {"HandleEventfd2Request", &EventRequestManager::HandleEventfd2Request, SYSCALL_eventfd2, validate_event<SYSCALL_eventfd2>},
        {"HandleSignalfdRequest", &EventRequestManager::HandleSignalfdRequest, SYSCALL_signalfd, validate_notification<SYSCALL_signalfd>},
        {"HandleSignalfd4Request", &EventRequestManager::HandleSignalfd4Request, SYSCALL_signalfd4, validate_notification<SYSCALL_signalfd4>},
    };

    if (caps->eventRequestManager != nullptr)
    {

        for (const auto& test : tests)
        {

            if ((caps->eventRequestManager->*(test.method))(nullptr) != LINUX_EINVAL)
            {
                expect(false, "event handler null-frame validation");
            }
        }
    }

    EventRequestManager unavailable(nullptr, nullptr);

    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};

        frame.syscall_number = test.syscallNumber;

        if ((unavailable.*(test.method))(&frame) != LINUX_ENOSYS)
        {
            expect(false, "event missing dependency handling");
        }
    }

    run_syscalls(caps->eventRequestManager, tests);
}

} // namespace request_selftest
