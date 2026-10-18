#include "RequestTestSupport.hpp"

namespace request_selftest
{
static bool resolve_runtime_identity(uint64_t* outPid, uint64_t* outTid)
{
    if (outPid == nullptr || outTid == nullptr)
    {
        return false;
    }

    Dispatcher* dispatcher = static_cast<Dispatcher*>(platform.dispacher);

    if (dispatcher == nullptr)
    {
        return false;
    }

    ResourceLayerCaps* resourceCaps = dispatcher->GetResourceLayerCaps();

    if (resourceCaps == nullptr || resourceCaps->processManager == nullptr || resourceCaps->taskManager == nullptr)
    {
        return false;
    }

    process_t* currentProcess = resourceCaps->processManager->GetCurrentProcess();
    task_t*    currentTask    = resourceCaps->taskManager->GetCurrentTask();

    if (currentProcess == nullptr || currentTask == nullptr)
    {
        return false;
    }

    *outPid = currentProcess->id;
    *outTid = currentTask->id;

    return true;
}

static bool validate_process_identity(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    uint64_t expectedPid = 0;
    uint64_t expectedTid = 0;

    if (!resolve_runtime_identity(&expectedPid, &expectedTid))
    {
        return false;
    }

    arch_syscall_frame_t dispatchedFrame = *(ctx->frame);

    dispatchedFrame.syscall_number = ctx->syscallNumber;

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&dispatchedFrame);

    if (ctx->syscallNumber == SYSCALL_getpid)
    {
        return equal(dispatchResult, expectedPid, "getpid returns the current process identity");
    }

    if (ctx->syscallNumber == SYSCALL_gettid)
    {
        return equal(dispatchResult, expectedTid, "gettid returns the current task identity");
    }

    return false;
}

static bool validate_getppid(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    Dispatcher* dispatcher = static_cast<Dispatcher*>(platform.dispacher);

    if (dispatcher == nullptr)
    {
        return false;
    }

    ResourceLayerCaps* resourceCaps = dispatcher->GetResourceLayerCaps();

    if (resourceCaps == nullptr || resourceCaps->processManager == nullptr)
    {
        return false;
    }

    process_t* currentProcess = resourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return false;
    }

    const uint64_t expectedPpid = currentProcess->hasParent ? currentProcess->parentId : 0;

    arch_syscall_frame_t dispatchedFrame = *(ctx->frame);

    dispatchedFrame.syscall_number = SYSCALL_getppid;

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&dispatchedFrame);

    return REQUEST_EXPECT(dispatchResult == expectedPpid);
}

struct lifecycle_fixture_t
{
    ResourceLayerCaps* resources;
    LogicLayerCaps*    logic;
    process_t*         original;
    process_t*         process;
    task_t*            caller;
    uint64_t           originalTid;
    bool               originalUser;
    char               originalName[16];
    uint64_t           originalFs;
    uint64_t           originalGs;
    bool               ready;

    static constexpr uint64_t CODE  = 0x400000;
    static constexpr uint64_t DATA  = 0x410000;
    static constexpr uint64_t STACK = 0x420000;

    lifecycle_fixture_t()
        : resources(nullptr), logic(nullptr), original(nullptr), process(nullptr), caller(nullptr), originalTid(0), originalUser(false), originalFs(0), originalGs(0), ready(false)
    {
        cpu_preemption_disable();

        Dispatcher* dispatcher = static_cast<Dispatcher*>(platform.dispacher);

        resources    = dispatcher->GetResourceLayerCaps();
        logic        = dispatcher->GetLogicLayerCaps();
        original     = resources->processManager->GetCurrentProcess();
        caller       = resources->taskManager->GetCurrentTask();
        originalTid  = caller->id;
        originalUser = caller->isUserTask;

        memcpy(originalName, caller->name, sizeof(originalName));

        (void) resources->taskManager->GetCurrentTlsBase(false, &originalFs);

        (void) resources->taskManager->GetCurrentTlsBase(true, &originalGs);

        virt_addr_space_t* space = resources->virtualMemoryManager->CreateUserAddressSpace();

        process = space != nullptr ? resources->processManager->CreateProcess(space) : nullptr;

        if (process == nullptr)
        {
            resources->virtualMemoryManager->DestroyUserAddressSpace(space);

            return;
        }

        process->ownsAddressSpace = true;

        resources->processManager->RemoveTask(original, caller);

        resources->processManager->AddTask(process, caller);

        resources->processManager->ActivateProcessAddressSpace(process);

        if (!resources->virtualMemoryManager->AllocateAnonymousPage(space, CODE, 7, false) || !resources->virtualMemoryManager->AllocateAnonymousPage(space, DATA, 3, false) ||
            !resources->virtualMemoryManager->AllocateAnonymousPage(space, STACK, 3, false))
        {
            return;
        }

        ready                  = arch_selftest_load_child(process, CODE);
        const uint32_t initial = 0x1234;

        ready = ready && safe_write_user(DATA, &initial, sizeof(initial));
    }

    ~lifecycle_fixture_t()
    {
        if (process == nullptr)
        {
            cpu_preemption_enable();

            return;
        }

        resources->taskManager->CleanupCurrentTask();

        caller->isUserTask = originalUser;

        resources->taskManager->SetCurrentIdentity(originalTid);

        memcpy(caller->name, originalName, sizeof(originalName));

        require_cleanup(resources->taskManager->SetCurrentTlsBase(false, originalFs), "restore caller FS base");

        require_cleanup(resources->taskManager->SetCurrentTlsBase(true, originalGs), "restore caller GS base");

        resources->processManager->RemoveTask(process, caller);

        require_cleanup(resources->processManager->AddTask(original, caller), "restore caller task membership");

        require_cleanup(resources->processManager->ActivateProcessAddressSpace(original), "restore caller address space");

        require_cleanup(caller->id == originalTid && caller->isUserTask == originalUser && memcmp(caller->name, originalName, sizeof(originalName)) == 0,
            "restore caller identity and name");

        for (size_t index = 0; index < resources->processManager->GetCapacity(); ++index)
        {
            process_t* child = &resources->processManager->GetProcesses()[index];

            if (child->allocated && child->hasParent && child->parentId == process->id)
            {
                child->waitConsumed = true;

                logic->scheduler->KillProcess(index, 0);
            }
        }

        process->waitConsumed = true;

        logic->scheduler->KillProcess(process->id, 0);

        cpu_preemption_enable();
    }

    arch_syscall_frame_t Frame(uint64_t number) const
    {
        arch_syscall_frame_t frame = {};

        frame.syscall_number = number;
        arch_selftest_user_frame(&frame, CODE, STACK + PAGE_SIZE);
        return frame;
    }

    uint64_t Spawn() const
    {
        arch_syscall_frame_t frame = Frame(SYSCALL_fork);

        return dispatcher_dispatch_syscall(&frame);
    }

    bool Reap(uint64_t pid) const
    {
        arch_syscall_frame_t frame = Frame(SYSCALL_wait4);

        frame.arg0              = pid;
        frame.arg1              = DATA + 64;
        frame.arg2              = 1; // WNOHANG: a missing child exit must not hang the suite.
        const uint64_t deadline = coordination_deadline();

        for (;;)
        {
            const uint64_t result = dispatcher_dispatch_syscall(&frame);

            if (result != 0)
            {
                int32_t status = 0;

                return equal(result, pid, "wait4 reaps the expected child") && expect(safe_read_user(DATA + 64, &status, sizeof(status)), "read child exit status") &&
                       equal(status, 37 << 8, "child exits with fixture status");
            }

            uint64_t now = 0;

            if (!arch_time_read_monotonic(&now) || now >= deadline)
            {
                return expect(false, "child exits within the two-second coordination deadline");
            }

            if (!equal(logic->scheduler->SleepUntil(now + 1000000), 0, "yield to child before retrying wait4"))
            {
                return false;
            }
        }
    }
};

static uint64_t lifecycle_clone(lifecycle_fixture_t& fixture, uint64_t number, uint64_t flags, uint64_t childTid = 0, uint64_t exitSignal = 17)
{
    process_clone_arguments_t arguments = {};

    arguments.flags      = flags;
    arguments.exitSignal = (flags & 0x10000) != 0 ? 0 : exitSignal;
    arguments.stack      = lifecycle_fixture_t::STACK;
    arguments.stackSize  = PAGE_SIZE;
    arguments.childTid   = childTid;

    arch_syscall_frame_t frame = fixture.Frame(number);

    if (number == SYSCALL_clone)
    {
        frame.arg0 = flags | arguments.exitSignal;
        frame.arg1 = arguments.stack + arguments.stackSize;
        frame.arg3 = arguments.childTid;
    }
    else
    {
        if (!safe_write_user(lifecycle_fixture_t::DATA + 256, &arguments, sizeof(arguments)))
        {
            return LINUX_EFAULT;
        }

        frame.arg0 = lifecycle_fixture_t::DATA + 256;
        frame.arg1 = sizeof(arguments);
    }

    return dispatcher_dispatch_syscall(&frame);
}

static bool lifecycle_resource_matrix(lifecycle_fixture_t& fixture, uint64_t number)
{
    auto call = [&](uint64_t syscall, uint64_t first = 0, uint64_t second = 0, uint64_t third = 0, uint64_t fourth = 0)
    {
        arch_syscall_frame_t frame = fixture.Frame(syscall);

        frame.arg0 = first;
        frame.arg1 = second;
        frame.arg2 = third;
        frame.arg3 = fourth;

        return dispatcher_dispatch_syscall(&frame);
    };

    // A dispatch claim keeps an exited child alive during the scheduler's
    // deferred retirement window. Exit status must not become waitable until
    // its task-owned descriptors and process resources have been released.
    const uint64_t deferredChild = lifecycle_clone(fixture, number, 0);
    TaskManager*   taskManager   = fixture.resources->taskManager;
    task_t*        deferredTask  = static_cast<int64_t>(deferredChild) > 0 ? taskManager->FindTask(deferredChild) : nullptr;
    const bool     retained      = deferredTask != nullptr && taskManager->RetainForDispatch(deferredTask);
    bool           retirementWait = expect(retained, "retain child dispatch claim to defer exit cleanup");

    if (retained)
    {
        fixture.logic->scheduler->KillProcess(deferredChild, 37);
        int32_t reportedPid = -1;
        retirementWait &= safe_write_user(lifecycle_fixture_t::DATA + 1024 + 16, &reportedPid, sizeof(reportedPid));
        retirementWait &= expect(call(SYSCALL_waitid, 1, deferredChild, lifecycle_fixture_t::DATA + 1024, 5) == 0,
            "waitid WEXITED|WNOHANG polls a child with deferred cleanup");
        retirementWait &= safe_read_user(lifecycle_fixture_t::DATA + 1024 + 16, &reportedPid, sizeof(reportedPid));
        retirementWait &= expect(reportedPid == 0, "waitid does not report exit status before child resources retire");
        const uint64_t earlyWait = call(SYSCALL_wait4, deferredChild, lifecycle_fixture_t::DATA + 64, 1);
        retirementWait &= expect(earlyWait == 0, "wait4 does not consume exit status before child resources retire");
        taskManager->ReleaseDispatchClaim(deferredTask);

        if (earlyWait == 0)
        {
            retirementWait &= fixture.Reap(deferredChild);
        }
        fixture.logic->scheduler->ReapExitedProcesses();
    }

    const uint64_t realtimeChild = lifecycle_clone(fixture, number, 0, 0, 34);
    bool supportedExitSignal = expect(static_cast<int64_t>(realtimeChild) > 0, "RT child-exit notification reserves bounded queue capacity");
    if (supportedExitSignal)
    {
        fixture.logic->scheduler->KillProcess(realtimeChild, 7);
        process_t* retired = fixture.resources->processManager->AcquireProcess(realtimeChild);
        if (retired != nullptr)
        {
            retired->waitConsumed = true;
            fixture.resources->processManager->ReleaseProcess(retired);
        }
        SignalDispositionGuard guard(fixture.process);
        signal_sender_t sender = {};
        supportedExitSignal &= expect(SignalManager::Peek(fixture.process, fixture.resources->taskManager->GetCurrentTask(), 1ULL << 33, &sender) == 34 && sender.pid == static_cast<int32_t>(realtimeChild) && sender.value == 7, "reserved child-exit record retains child and status");
        SignalManager::Consume(fixture.process, fixture.resources->taskManager->GetCurrentTask(), 34);
        guard.Release();
        fixture.logic->scheduler->ReapExitedProcesses();
    }

    const char         directory[] = "/clone-resource-sharing";
    VirtualFileSystem* files       = fixture.logic->virtualFileSystem;
    ProcessManager*    processes   = fixture.resources->processManager;

    // The boot cpio filesystem is read-only. Use a directory shipped in the
    // initramfs so cwd sharing is exercised without requiring a writable mount.
    vfs_path_t directoryPath = {};

    bool passed = retirementWait && supportedExitSignal && expect(files->ResolvePath({}, directory, &directoryPath) && directoryPath.dentry != nullptr && directoryPath.dentry->inode != nullptr && directoryPath.dentry->inode->type == INODE_DIRECTORY, "clone matrix directory exists in initramfs");

    uint8_t originalCode[64] = {};

    passed &= safe_read_user(lifecycle_fixture_t::CODE, originalCode, sizeof(originalCode));

    for (unsigned variant = 0; passed && variant < 16; ++variant)
    {
        const bool      thread       = variant >= 12;
        const bool      sharedVm     = variant < 8 || thread;
        const uint64_t  selection    = (thread ? variant - 12 : variant >= 8 ? variant - 8
                                                                             : variant) ^
                                       1U;
        const bool      shareFiles   = (selection & 1) != 0;
        const bool      shareFs      = (selection & 2) != 0;
        const bool      shareSignals = thread || (sharedVm && (selection & 4) != 0);
        const uint64_t  flags        = (sharedVm ? 0x100 : 0) | (shareFiles ? 0x400 : 0) | (shareFs ? 0x200 : 0) | (shareSignals ? 0x800 : 0) | (thread ? 0x10000 : 0);
        signal_action_t action       = {};

        passed &= safe_write_user(lifecycle_fixture_t::DATA + 512, &action, sizeof(action));

        passed &= call(SYSCALL_rt_sigaction, 10, lifecycle_fixture_t::DATA + 512, 0, 8) == 0;

        (void) call(SYSCALL_umask, 0022);

        file_t* file = files->Open({}, "/test.txt", 0);

        if (!expect(file != nullptr, "clone matrix opens a real file description"))
        {
            passed = false;

            break;
        }

        const int64_t fd = processes->AddFileDescriptor(fixture.process, file, FD_FLAG_NONE, dispatcher_vfs_file_operations());

        if (fd < 0)
        {
            files->Close(file);

            passed = false;

            break;
        }

        const int64_t keptFd = static_cast<int64_t>(call(SYSCALL_fcntl, fd, 0, 0));

        if (keptFd < 0)
        {
            (void) call(SYSCALL_close, fd);

            passed = false;

            break;
        }

        const bool observed = files->Retain(file);

        // Run real child syscalls so private thread accessors are exercised,
        // rather than merely inspecting the resource pointers from the parent.
        passed &= arch_selftest_load_resource_child(fixture.process, lifecycle_fixture_t::CODE, static_cast<uint32_t>(fd));

        const uint64_t originalAffinity  = fixture.caller->affinityMask;
        const int32_t  originalNice      = fixture.caller->baseNice;
        const uint64_t inheritedAffinity = 1ULL << arch_cpu_id();
        passed &= fixture.logic->scheduler->SetAffinity(fixture.caller, inheritedAffinity) == 0;
        passed &= fixture.logic->scheduler->SetNice(fixture.caller, 7) == 0;
        const uint64_t id = lifecycle_clone(fixture, number, flags);
        passed &= fixture.logic->scheduler->SetAffinity(fixture.caller, originalAffinity) == 0;
        passed &= fixture.logic->scheduler->SetNice(fixture.caller, originalNice) == 0;
        task_t* child = static_cast<int64_t>(id) > 0 ? fixture.resources->taskManager->FindTask(id) : nullptr;

        if (!expect(observed && child != nullptr, "clone accepts each supported resource-sharing combination"))
        {
            (void) call(SYSCALL_close, fd);

            (void) call(SYSCALL_close, keptFd);

            if (observed)
            {
                files->Close(file);
            }

            passed = false;

            break;
        }

        passed &= expect(child->affinityMask == inheritedAffinity && child->baseNice == 7, "clone inherits restricted affinity and nondefault base priority");

        passed &= expect((child->descriptorTable == fixture.process->Files()) == shareFiles, "CLONE_FILES selects shared table independently of thread group");

        passed &= expect((child->filesystemContext == fixture.process->Fs()) == shareFs, "CLONE_FS selects shared context independently of thread group");

        passed &= expect((child->process->signalDispositions == fixture.process->signalDispositions) == shareSignals,
            "CLONE_SIGHAND selects shared dispositions independently of process identity");

        passed &= call(SYSCALL_fcntl, fd, 2, 1) == 0;

        passed &= expect(child->descriptorTable->entries[fd].flags == (shareFiles ? FD_FLAG_CLOEXEC : FD_FLAG_NONE),
            "shared descriptor flag changes propagate; private flags stay isolated");

        passed &= processes->EnsureFileDescriptorCapacity(fixture.process, 63);

        passed &= expect(!shareFiles || child->descriptorTable->count == fixture.process->Files()->count, "shared descriptor table growth publishes relocated entries");

        (void) call(SYSCALL_umask, 0077);

        passed &= expect(child->filesystemContext->umask == (shareFs ? 0077U : 0022U), "shared umask propagates; private umask stays isolated");

        passed &= safe_write_user(lifecycle_fixture_t::DATA + 640, directory, sizeof(directory));

        passed &= call(SYSCALL_chdir, lifecycle_fixture_t::DATA + 640) == 0;

        passed &= expect(strcmp(child->filesystemContext->cwdPath, shareFs ? directory : "/") == 0, "shared chdir propagates; private cwd stays isolated");

        __atomic_fetch_or(&child->process->signalPending, 1ULL << 9, __ATOMIC_RELEASE);

        __atomic_fetch_or(&child->signalPending, 1ULL << 9, __ATOMIC_RELEASE);

        action.handler = 1;

        passed &= safe_write_user(lifecycle_fixture_t::DATA + 512, &action, sizeof(action));

        passed &= call(SYSCALL_rt_sigaction, 10, lifecycle_fixture_t::DATA + 512, 0, 8) == 0;

        passed &= expect(child->process->Signals()->actions[9].handler == (shareSignals ? 1U : 0U), "shared sigaction propagates; private dispositions stay isolated");

        passed &= expect(((child->process->signalPending & (1ULL << 9)) == 0) == shareSignals && ((child->signalPending & (1ULL << 9)) == 0) == shareSignals,
            "ignoring a shared disposition discards pending signals in every sharing owner");

        __atomic_fetch_and(&child->process->signalPending, ~(1ULL << 9), __ATOMIC_RELEASE);

        __atomic_fetch_and(&child->signalPending, ~(1ULL << 9), __ATOMIC_RELEASE);

        passed &= call(SYSCALL_close, fd) == 0;

        passed &= expect((child->descriptorTable->entries[fd].file == nullptr) == shareFiles, "shared close propagates; private descriptors remain open");

        passed &= expect(file->refCount == (shareFiles ? 2U : 4U), "private child retains the file after its parent closes one descriptor");

        if (thread)
        {
            const uint64_t sequence = __atomic_load_n(&fixture.process->childSequence, __ATOMIC_ACQUIRE);

            passed &= fixture.logic->scheduler->WaitForEvent(&fixture.process->childSequence, sequence, coordination_deadline(), true) == 0;

            fixture.logic->scheduler->ReapExitedProcesses();
        }
        else
        {
            passed &= fixture.Reap(id);
        }

        passed &= expect(file->refCount == 2, "child exit releases only its owned open file references");

        passed &= expect(processes->GetFileDescriptor(fixture.process, keptFd) != nullptr, "child exit preserves the surviving owner's shared descriptors");

        uint64_t closeResult = UINT64_MAX;
        uint64_t priorUmask  = UINT64_MAX;

        if (sharedVm)
        {
            passed &= safe_read_user(lifecycle_fixture_t::DATA + 32, &closeResult, sizeof(closeResult));

            passed &= safe_read_user(lifecycle_fixture_t::DATA + 40, &priorUmask, sizeof(priorUmask));

            passed &= expect(closeResult == (shareFiles ? LINUX_EBADF : 0), "child close resolves the calling task's shared or private table");

            passed &= expect(priorUmask == (shareFs ? 0077U : 0022U), "child umask resolves the calling task's filesystem context");
        }

        passed &= call(SYSCALL_close, keptFd) == 0;

        passed &= expect(file->refCount == 1, "last descriptor owner closes after the child exits");

        files->Close(file);

        passed &= expect(fixture.process->Files()->entries[fd].file == nullptr && fixture.process->Fs()->umask == (shareFs ? 0002U : 0077U),
            "child filesystem changes propagate only to shared owners");

        const char root[] = "/";

        passed &= safe_write_user(lifecycle_fixture_t::DATA + 640, root, sizeof(root));

        passed &= call(SYSCALL_chdir, lifecycle_fixture_t::DATA + 640) == 0;
    }

    (void) call(SYSCALL_umask, 0022);

    passed &= copy_to_process_virtual(fixture.process->addressSpace, lifecycle_fixture_t::CODE, originalCode, sizeof(originalCode));

    // Existing validation remains authoritative for malformed and excluded flags.
    passed &= expect(lifecycle_clone(fixture, number, 0x800) == LINUX_EINVAL, "SIGHAND without VM is invalid");

    passed &= expect(lifecycle_clone(fixture, number, 0x10100) == LINUX_EINVAL, "THREAD without SIGHAND is invalid");

    passed &= expect(lifecycle_clone(fixture, number, 0x100 | 0x20000000) == LINUX_EOPNOTSUPP, "namespace clone flags remain explicitly unsupported");

    passed &= expect(lifecycle_clone(fixture, number, 0x100 | 0x1000) == LINUX_EOPNOTSUPP, "pidfd clone flag remains explicitly unsupported");

    // The fixture pins this CPU. Refuse publication through the scheduler's
    // existing ownership check, after all clone resources have been prepared.
    const uint32_t marker = 0x55aa1234;

    passed &= safe_write_user(lifecycle_fixture_t::DATA + 768, &marker, sizeof(marker));

    const uint32_t tableReferences      = fixture.process->Files()->references;
    const uint32_t filesystemReferences = fixture.process->Fs()->references;
    const uint32_t signalReferences     = fixture.process->signalDispositions->references;
    const uint32_t memoryUsers          = fixture.process->memoryUsers;
    const size_t   taskCount            = fixture.resources->taskManager->CountTasks();
    const int16_t  ownerCpu             = fixture.process->ownerCpu;

    fixture.process->ownerCpu = static_cast<int16_t>(BOOT_SMP_MAX_CPUS);

    const uint64_t rejected = lifecycle_clone(fixture, number, 0x10f00 | 0x200000, lifecycle_fixture_t::DATA + 768);

    fixture.process->ownerCpu = ownerCpu;

    uint32_t afterFailure = 0;

    passed &= expect(rejected == LINUX_EAGAIN, "failed publication reports clone failure");

    passed &= safe_read_user(lifecycle_fixture_t::DATA + 768, &afterFailure, sizeof(afterFailure));

    passed &= expect(afterFailure == marker, "failed clone cancels clear-child-tid without modifying user memory");

    passed &= expect(fixture.process->Files()->references == tableReferences && fixture.process->Fs()->references == filesystemReferences &&
                         fixture.process->signalDispositions->references == signalReferences && fixture.process->memoryUsers == memoryUsers,
        "failed publication rolls back resource and robust-owner references");

    passed &= expect(fixture.resources->taskManager->CountTasks() == taskCount && fixture.process->tasks == fixture.caller && fixture.caller->next == nullptr,
        "failed publication removes its task and thread-group membership");

    return passed;
}

struct lifecycle_shared_observer_t
{
    lifecycle_fixture_t& fixture;
    process_t*           process;

    explicit lifecycle_shared_observer_t(lifecycle_fixture_t& owner) : fixture(owner), process(nullptr)
    {
        const uint64_t id = lifecycle_clone(fixture, SYSCALL_clone, 0xf00);

        if (static_cast<int64_t>(id) > 0)
        {
            process = fixture.resources->processManager->AcquireProcess(id);
        }
    }

    ~lifecycle_shared_observer_t()
    {
        if (process != nullptr)
        {
            const uint64_t id = process->id;

            process->waitConsumed = true;

            (void) fixture.logic->scheduler->KillProcess(id, 0);

            fixture.resources->processManager->ReleaseProcess(process);

            fixture.logic->scheduler->ReapExitedProcesses();
        }
    }
};

static bool validate_process_lifecycle(const syscall_case_t* context)
{
    if (!arch_task_context_supported())
    {
        return false;
    }
    lifecycle_fixture_t fixture;

    if (!fixture.ready)
    {
        return false;
    }

    const uint64_t       number     = context->syscallNumber;
    arch_syscall_frame_t frame      = fixture.Frame(number);
    ProcessController*   controller = fixture.logic->processController;

    if (number == SYSCALL_exit || number == SYSCALL_exit_group)
    {
        // Run the non-returning syscall in a disposable user child. The harness
        // must stay live, and wait4 must observe the child's exit status.
        if (!arch_selftest_load_child(fixture.process, lifecycle_fixture_t::CODE, static_cast<uint32_t>(number)))
        {
            return false;
        }

        const uint64_t pid = fixture.Spawn();

        return static_cast<int64_t>(pid) > 0 && fixture.Reap(pid);
    }

    if (number == SYSCALL_fork)
    {
        const uint64_t first     = fixture.Spawn();
        uint32_t       unchanged = 0;

        if (static_cast<int64_t>(first) <= 0 || !fixture.Reap(first) || !safe_read_user(lifecycle_fixture_t::DATA, &unchanged, sizeof(unchanged)) || unchanged != 0x1234)
        {
            return false;
        }

        const uint64_t pid = dispatcher_dispatch_syscall(&frame);

        if (static_cast<int64_t>(pid) < 0)
        {
            return false;
        }

        process_t* child = fixture.resources->processManager->AcquireProcess(pid);

        if (child == nullptr)
        {
            return false;
        }

        const phys_addr_t before = vmm_virt_to_phys(lifecycle_fixture_t::DATA, fixture.process->addressSpace);
        bool              passed = before == vmm_virt_to_phys(lifecycle_fixture_t::DATA, child->addressSpace);
        const uint32_t    value  = 0xabcd;

        passed = passed && safe_write_user(lifecycle_fixture_t::DATA, &value, sizeof(value));
        passed = passed && before != vmm_virt_to_phys(lifecycle_fixture_t::DATA, fixture.process->addressSpace);

        fixture.resources->processManager->ReleaseProcess(child);

        passed = fixture.Reap(pid) && passed;

        uint32_t parentValue = 0;

        return passed && safe_read_user(lifecycle_fixture_t::DATA, &parentValue, sizeof(parentValue)) && parentValue == value;
    }

    if (number == SYSCALL_vfork)
    {
        const uint64_t pid = dispatcher_dispatch_syscall(&frame);

        uint32_t value = 0;

        return static_cast<int64_t>(pid) > 0 && safe_read_user(lifecycle_fixture_t::DATA, &value, sizeof(value)) && value == 0x5678 && fixture.Reap(pid);
    }

    if (number == SYSCALL_clone || number == SYSCALL_clone3)
    {
        if (!lifecycle_resource_matrix(fixture, number))
        {
            return false;
        }

        process_clone_arguments_t arguments = {};

        arguments.flags     = 0x10f00 | 0x80000 | 0x100000 | 0x200000 | 0x1000000;
        arguments.stack     = lifecycle_fixture_t::STACK;
        arguments.stackSize = PAGE_SIZE;
        arguments.parentTid = lifecycle_fixture_t::DATA + 16;
        arguments.childTid  = lifecycle_fixture_t::DATA + 20;
        arguments.tls       = lifecycle_fixture_t::DATA + 128;

        if (number == SYSCALL_clone)
        {
            frame.arg0 = arguments.flags;
            frame.arg1 = arguments.stack + arguments.stackSize;
            frame.arg2 = arguments.parentTid;
            frame.arg3 = arguments.childTid;
            frame.arg4 = arguments.tls;
        }
        else
        {
            if (!safe_write_user(lifecycle_fixture_t::DATA + 256, &arguments, sizeof(arguments)))
            {
                return false;
            }

            frame.arg0 = lifecycle_fixture_t::DATA + 256;
            frame.arg1 = sizeof(arguments);
        }

        const uint64_t tid = dispatcher_dispatch_syscall(&frame);

        if (static_cast<int64_t>(tid) <= 0)
        {
            return false;
        }

        uint32_t written = 0;
        bool     passed  = safe_read_user(arguments.parentTid, &written, sizeof(written)) && written == tid;
        task_t*  child   = fixture.resources->taskManager->FindTask(tid);

        passed = passed && child != nullptr && child->process == fixture.process && arch_selftest_context_tls_matches(&child->taskContext, arguments.tls);

        const uint64_t sequence = __atomic_load_n(&fixture.process->childSequence, __ATOMIC_ACQUIRE);
        const uint64_t waited   = fixture.logic->scheduler->WaitForEvent(&fixture.process->childSequence, sequence, coordination_deadline(), true);
        uint32_t       marker   = 0;

        passed = waited == 0 && passed && safe_read_user(arguments.childTid, &written, sizeof(written)) && written == 0;
        passed = passed && safe_read_user(lifecycle_fixture_t::DATA, &marker, sizeof(marker)) && marker == 0x5678;

        fixture.logic->scheduler->ReapExitedProcesses();

        return expect(passed, "clone thread tid publication, execution and retirement");
    }

    if (number == SYSCALL_wait4 || number == SYSCALL_waitid)
    {
        const uint64_t pid = fixture.Spawn();

        if (static_cast<int64_t>(pid) <= 0)
        {
            return false;
        }

        frame.arg0 = number == SYSCALL_waitid ? 1 : pid;
        frame.arg1 = number == SYSCALL_waitid ? pid : lifecycle_fixture_t::DATA + 64;
        frame.arg2 = number == SYSCALL_waitid ? lifecycle_fixture_t::DATA + 64 : 1;
        frame.arg3 = number == SYSCALL_waitid ? 5 : 0;

        bool passed = dispatcher_dispatch_syscall(&frame) == 0;

        if (number == SYSCALL_wait4)
        {
            passed     = passed && fixture.logic->signalManager->Send(static_cast<int32_t>(pid), -1, 19) == 0;
            frame.arg2 = 2;

            passed = passed && dispatcher_dispatch_syscall(&frame) == pid;

            int32_t state = 0;

            passed     = passed && safe_read_user(frame.arg1, &state, sizeof(state)) && state == ((19 << 8) | 127);
            passed     = passed && fixture.logic->signalManager->Send(static_cast<int32_t>(pid), -1, 18) == 0;
            frame.arg2 = 8;

            passed = passed && dispatcher_dispatch_syscall(&frame) == pid;

            passed     = passed && safe_read_user(frame.arg1, &state, sizeof(state)) && state == 0xffff;
            frame.arg1 = CANONICAL_KERNEL_BASE;
            frame.arg2 = 0;

            passed = passed && dispatcher_dispatch_syscall(&frame) == LINUX_EFAULT;

            return fixture.Reap(pid) && passed;
        }

        frame.arg3 = 4 | 0x1000000;

        passed = passed && dispatcher_dispatch_syscall(&frame) == 0;

        int32_t info[8] = {};

        passed =
            passed && safe_read_user(lifecycle_fixture_t::DATA + 64, info, sizeof(info)) && info[0] == 17 && info[2] == 1 && info[4] == static_cast<int32_t>(pid) && info[6] == 37;
        frame.arg3 = 4;

        passed = passed && dispatcher_dispatch_syscall(&frame) == 0;

        return passed && dispatcher_dispatch_syscall(&frame) == static_cast<uint64_t>(-10);
    }

    if (number == SYSCALL_set_tid_address)
    {
        const uint32_t marker = 123;

        (void) safe_write_user(lifecycle_fixture_t::DATA, &marker, sizeof(marker));

        frame.arg0 = lifecycle_fixture_t::DATA;

        bool passed = dispatcher_dispatch_syscall(&frame) == fixture.caller->id;

        passed = passed && fixture.caller->clearChildTid == lifecycle_fixture_t::DATA;

        fixture.resources->taskManager->CleanupCurrentTask();

        uint32_t value = marker;

        return passed && safe_read_user(lifecycle_fixture_t::DATA, &value, sizeof(value)) && value == 0;
    }

    if (number == SYSCALL_execve)
    {
        const char     path[]     = "/test_syscall_exit.elf";
        const char     argument[] = "lifecycle";
        const uint64_t pointers[] = {lifecycle_fixture_t::DATA + 128, 0};

        (void) safe_write_user(lifecycle_fixture_t::DATA, path, sizeof(path));

        (void) safe_write_user(lifecycle_fixture_t::DATA + 128, argument, sizeof(argument));

        (void) safe_write_user(lifecycle_fixture_t::DATA + 256, pointers, sizeof(pointers));

        frame.arg0                 = lifecycle_fixture_t::DATA;
        frame.arg1                 = lifecycle_fixture_t::DATA + 256;
        fixture.caller->isUserTask = true;

        file_t* sharedFile = fixture.logic->virtualFileSystem->Open({}, "/test.txt", 0);

        if (!expect(sharedFile != nullptr, "exec sharing opens a real descriptor"))
        {
            fixture.caller->isUserTask = false;

            return false;
        }

        const int64_t sharedFd = fixture.resources->processManager->AddFileDescriptor(fixture.process, sharedFile, FD_FLAG_CLOEXEC, dispatcher_vfs_file_operations());

        if (sharedFd < 0)
        {
            fixture.logic->virtualFileSystem->Close(sharedFile);

            fixture.caller->isUserTask = false;

            return false;
        }

        signal_action_t caught  = {lifecycle_fixture_t::CODE, 0x04000000, lifecycle_fixture_t::CODE, 0};
        signal_action_t ignored = {1, 0, 0, 0};

        if (fixture.logic->signalManager->Action(10, &caught, nullptr) != 0 || fixture.logic->signalManager->Action(12, &ignored, nullptr) != 0)
        {
            fixture.caller->isUserTask = false;

            return false;
        }

        lifecycle_shared_observer_t observer(fixture);

        if (!expect(observer.process != nullptr, "clone creates an external exec resource sharer"))
        {
            fixture.caller->isUserTask = false;

            return false;
        }

        descriptor_table_t*    oldTable                = fixture.process->Files();
        filesystem_context_t*  oldFilesystem           = fixture.process->Fs();
        signal_dispositions_t* oldSignals              = fixture.process->signalDispositions;
        const uint32_t         oldTableReferences      = oldTable->references;
        const uint32_t         oldFilesystemReferences = oldFilesystem->references;
        const uint32_t         oldSignalReferences     = oldSignals->references;
        const uint64_t         oldRoot                 = fixture.process->addressSpace->pt;
        arch_syscall_frame_t   invalid                 = frame;

        invalid.arg0 = CANONICAL_KERNEL_BASE;

        if (dispatcher_dispatch_syscall(&invalid) != LINUX_EFAULT || fixture.process->addressSpace->pt != oldRoot)
        {
            fixture.caller->isUserTask = false;

            return false;
        }

        const char nonExecutable[] = "/test.txt";

        if (!safe_write_user(lifecycle_fixture_t::DATA, nonExecutable, sizeof(nonExecutable)) ||
            !expect(dispatcher_dispatch_syscall(&frame) == LINUX_EACCES && fixture.process->addressSpace->pt == oldRoot,
                "exec rejects nonexecutable archive file without replacing address space") ||
            !safe_write_user(lifecycle_fixture_t::DATA, path, sizeof(path)))
        {
            fixture.caller->isUserTask = false;

            return false;
        }

        if (!expect(fixture.process->Files() == oldTable && fixture.process->Fs() == oldFilesystem && fixture.process->signalDispositions == oldSignals,
                "failed exec preserves shared resource identities") ||
            !expect(oldTable->references == oldTableReferences && oldFilesystem->references == oldFilesystemReferences && oldSignals->references == oldSignalReferences,
                "failed exec preserves ownership counts") ||
            !expect(oldTable->entries[sharedFd].file == sharedFile && oldSignals->actions[9].handler == caught.handler,
                "failed exec preserves CLOEXEC descriptors and caught handlers"))
        {
            fixture.caller->isUserTask = false;

            return false;
        }

        const uint64_t result = dispatcher_dispatch_syscall(&frame);

        fixture.caller->isUserTask = false;

        if (!expect(result == 0, "exec succeeds") || !expect(fixture.process->addressSpace->pt != oldRoot, "exec replaces address space") || !expect((arch_signal_stack_pointer(&frame) & 15) == 0 && arch_frame_instruction_pointer(&frame) != lifecycle_fixture_t::CODE, "exec entry and stack layout"))
        {
            return false;
        }

        if (!expect(fixture.process->Files() != oldTable && fixture.process->Fs() != oldFilesystem && fixture.process->signalDispositions != oldSignals,
                "successful exec unshares descriptor, filesystem and signal contexts") ||
            !expect(observer.process->descriptorTable->entries[sharedFd].file == sharedFile && observer.process->descriptorTable->entries[sharedFd].flags == FD_FLAG_CLOEXEC,
                "exec CLOEXEC preserves an external sharer's descriptor") ||
            !expect(fixture.resources->processManager->GetFileDescriptor(fixture.process, sharedFd) == nullptr, "exec closes CLOEXEC only in its private table") ||
            !expect(fixture.process->Signals()->actions[9].handler == 0 && observer.process->Signals()->actions[9].handler == caught.handler,
                "exec resets caught dispositions without changing external sharers") ||
            !expect(fixture.process->Signals()->actions[11].handler == 1, "exec preserves ignored dispositions"))
        {
            return false;
        }

        const uint32_t oldUmask = observer.process->filesystemContext->umask;

        (void) fixture.resources->processManager->ExchangeUmask(fixture.process, oldUmask ^ 0077U);

        if (!expect(observer.process->filesystemContext->umask == oldUmask, "filesystem mutation after exec stays private"))
        {
            return false;
        }

        uint64_t words[4] = {};

        if (!safe_read_user(arch_signal_stack_pointer(&frame), words, sizeof(words)) || words[0] != 1 || words[2] != 0 || words[3] != 0)
        {
            return false;
        }

        char copied[sizeof(argument)] = {};

        return safe_read_user(words[1], copied, sizeof(copied)) && strcmp(copied, argument) == 0;
    }

    if (number == SYSCALL_getpgid || number == SYSCALL_getpgrp || number == SYSCALL_getsid)
    {
        const uint64_t expected = number == SYSCALL_getsid ? fixture.process->sessionId : fixture.process->groupId;

        bool passed = dispatcher_dispatch_syscall(&frame) == expected;

        if (number != SYSCALL_getpgrp)
        {
            frame.arg0 = INT32_MAX;

            passed = passed && dispatcher_dispatch_syscall(&frame) == LINUX_ESRCH;
        }

        return passed;
    }

    if (number == SYSCALL_setpgid)
    {
        const uint64_t child = fixture.Spawn();

        frame.arg0 = child;
        frame.arg1 = 0;

        bool passed = static_cast<int64_t>(child) > 0 && dispatcher_dispatch_syscall(&frame) == 0;

        passed     = passed && controller->Group(0, static_cast<int32_t>(child)) == child;
        frame.arg1 = UINT32_MAX;

        return passed && dispatcher_dispatch_syscall(&frame) == LINUX_EINVAL && fixture.Reap(child);
    }

    if (number == SYSCALL_setsid)
    {
        bool passed = dispatcher_dispatch_syscall(&frame) == fixture.process->id;

        passed = passed && fixture.process->sessionId == fixture.process->id && fixture.process->groupId == fixture.process->id;

        return passed && dispatcher_dispatch_syscall(&frame) == static_cast<uint64_t>(-1);
    }

    return false;
}

void test_process_manager(RequestLayerCaps* caps)
{
    const auto  lifecycle_coverage = arch_task_context_supported() ? syscall_coverage_t::Tested : syscall_coverage_t::Unsupported;
    const char* lifecycle_reason   = arch_task_context_supported() ? nullptr : "user lifecycle fixture requires x86-64 machine code";

    static const syscall_test_t<ProcessRequestManager> exitTests[] = {
        {"HandleExitRequest", &ProcessRequestManager::HandleExitRequest, SYSCALL_exit, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleExit_groupRequest", &ProcessRequestManager::HandleExit_groupRequest, SYSCALL_exit_group, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
    };
    run_syscalls(caps->processRequestManager, exitTests);

    static const syscall_test_t<ProcessRequestManager> tests[] = {
        {"HandleForkRequest", &ProcessRequestManager::HandleForkRequest, SYSCALL_fork, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleVforkRequest", &ProcessRequestManager::HandleVforkRequest, SYSCALL_vfork, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleCloneRequest", &ProcessRequestManager::HandleCloneRequest, SYSCALL_clone, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleClone3Request", &ProcessRequestManager::HandleClone3Request, SYSCALL_clone3, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleExecveRequest", &ProcessRequestManager::HandleExecveRequest, SYSCALL_execve, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleWait4Request", &ProcessRequestManager::HandleWait4Request, SYSCALL_wait4, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleWaitidRequest", &ProcessRequestManager::HandleWaitidRequest, SYSCALL_waitid, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleSet_tid_addressRequest", &ProcessRequestManager::HandleSet_tid_addressRequest, SYSCALL_set_tid_address, validate_process_lifecycle, lifecycle_coverage,
            lifecycle_reason},
        {"HandleGettidRequest", &ProcessRequestManager::HandleGettidRequest, SYSCALL_gettid, validate_process_identity},
        {"HandleGetpidRequest", &ProcessRequestManager::HandleGetpidRequest, SYSCALL_getpid, validate_process_identity},
        {"HandleGetppidRequest", &ProcessRequestManager::HandleGetppidRequest, SYSCALL_getppid, validate_getppid},
        {"HandleGetpgidRequest", &ProcessRequestManager::HandleGetpgidRequest, SYSCALL_getpgid, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleGetpgrpRequest", &ProcessRequestManager::HandleGetpgrpRequest, SYSCALL_getpgrp, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleSetpgidRequest", &ProcessRequestManager::HandleSetpgidRequest, SYSCALL_setpgid, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleGetsidRequest", &ProcessRequestManager::HandleGetsidRequest, SYSCALL_getsid, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
        {"HandleSetsidRequest", &ProcessRequestManager::HandleSetsidRequest, SYSCALL_setsid, validate_process_lifecycle, lifecycle_coverage, lifecycle_reason},
    };

    run_syscalls(caps->processRequestManager, tests);
}

} // namespace request_selftest
