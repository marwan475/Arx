#include "RequestTestSupport.hpp"

namespace request_selftest
{
static bool validate_umask(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    constexpr uint64_t firstMask  = 0022;
    constexpr uint64_t secondMask = 0077;

    arch_syscall_frame_t firstFrame = {};

    firstFrame.syscall_number = SYSCALL_umask;
    firstFrame.arg0           = firstMask;

    const uint64_t oldMask = dispatcher_dispatch_syscall(&firstFrame);

    arch_syscall_frame_t secondFrame = firstFrame;

    secondFrame.arg0 = secondMask | ~0777ULL;

    const uint64_t previousSecond = dispatcher_dispatch_syscall(&secondFrame);

    arch_syscall_frame_t restoreFrame = firstFrame;

    restoreFrame.arg0 = oldMask;

    const uint64_t previousRestore = dispatcher_dispatch_syscall(&restoreFrame);

    return REQUEST_EXPECT(previousSecond == firstMask && previousRestore == secondMask);
}

static bool test_system_tls(process_t* process)
{
#if defined(__x86_64__)
    uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    Dispatcher*  dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    TaskManager* tasks      = dispatcher->GetResourceLayerCaps()->taskManager;
    uint64_t     savedFs = 0, savedGs = 0;

    if (!tasks->GetCurrentTlsBase(false, &savedFs) || !tasks->GetCurrentTlsBase(true, &savedGs))
    {
        return false;
    }

    bool ok = true;

    for (uint64_t gs = 0; gs < 2; ++gs)
    {
        const uint64_t set   = gs ? 0x1001 : 0x1002;
        const uint64_t get   = gs ? 0x1004 : 0x1003;
        const uint64_t value = base + 128 + gs * 8;

        ok &= expect(syscall_call(SYSCALL_arch_prctl, set, value) == 0, "arch_prctl sets TLS base");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, get, base + PAGE_SIZE - 4) == 0, "arch_prctl cross-page output");

        uint64_t actual = 0;

        fixture_load(process, base + PAGE_SIZE - 4, &actual, sizeof(actual));

        ok &= expect(actual == value, "arch_prctl TLS roundtrip");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, set, 1ULL << 47) == (uint64_t) -1, "arch_prctl rejects non-user bases");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, get, base) == 0, "arch_prctl reads after failed set");

        fixture_load(process, base, &actual, sizeof(actual));

        ok &= expect(actual == value, "arch_prctl failed set preserves hardware state");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, get, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "arch_prctl rejects kernel output");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, get, 0) == LINUX_EFAULT, "arch_prctl rejects null output");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, set, 0) == 0, "arch_prctl permits zero base");
    }

    ok &= expect(syscall_call(SYSCALL_arch_prctl, 0) == LINUX_EINVAL, "arch_prctl invalid operation");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_arch_prctl, 0x1003, base) == LINUX_EFAULT, "arch_prctl readonly output");

    require_cleanup(tasks->SetCurrentTlsBase(false, savedFs), "restore caller FS base");

    require_cleanup(tasks->SetCurrentTlsBase(true, savedGs), "restore caller GS base");

    return ok;
#else
    (void) process;

    return expect(syscall_call(SYSCALL_arch_prctl) == LINUX_ENOSYS, "arch_prctl x86-only");
#endif
}

static bool test_system_name(process_t* process)
{
    uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    Dispatcher*  dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    TaskManager* tasks      = dispatcher->GetResourceLayerCaps()->taskManager;
    char         saved[16] = {}, name[16] = {};

    if (!tasks->GetCurrentName(saved))
    {
        return false;
    }

    const char input[] = "abcdefghijklmnopq";

    fixture_store(process, base, input, sizeof(input));

    bool ok = expect(syscall_call(SYSCALL_prctl, 15, base) == 0, "prctl sets name");

    ok &= expect(syscall_call(SYSCALL_prctl, 16, base + PAGE_SIZE - 8) == 0, "prctl cross-page get name");

    fixture_load(process, base + PAGE_SIZE - 8, name, sizeof(name));

    ok &= expect(memcmp(name, input, 15) == 0 && name[15] == 0, "prctl truncates name to 15 bytes");

    ok &= expect(syscall_call(SYSCALL_prctl, 15, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "prctl invalid set buffer");

    ok &= expect(syscall_call(SYSCALL_prctl, 16, base) == 0, "prctl reads after failed set");

    fixture_load(process, base, name, sizeof(name));

    ok &= expect(memcmp(name, input, 15) == 0 && name[15] == 0, "prctl failed set preserves task name");

    ok &= expect(syscall_call(SYSCALL_prctl, 16, 0) == LINUX_EFAULT, "prctl null get buffer");

    ok &= expect(syscall_call(SYSCALL_prctl, 15, base + 2 * PAGE_SIZE - 1) == 0, "prctl short NUL string at mapped boundary");

    ok &= expect(syscall_call(SYSCALL_prctl, 16, base) == 0 && *fixture_bytes(process, base) == 0, "prctl empty name");

    ok &= expect(syscall_call(SYSCALL_prctl, 0x7fffffff) == LINUX_EOPNOTSUPP, "prctl unsupported operation");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_prctl, 16, base) == LINUX_EFAULT, "prctl readonly output");

    tasks->SetCurrentName(saved);

    return ok;
}

static bool test_system_limits(process_t* process, uint64_t number)
{
    uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    const uint64_t       output   = base + PAGE_SIZE - 8;
    process_file_limit_t original = {}, actual = {};

    bool ok = expect(syscall_call(SYSCALL_getrlimit, 7, output) == 0, "getrlimit cross-page snapshot");

    fixture_load(process, output, &original, sizeof(original));

    ok &= expect(original.current == 1024 && original.maximum == ProcessManager::MAX_FILE_DESCRIPTORS, "getrlimit default limits");

    ResourceLayerCaps* resources = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps();

    virt_addr_space_t* peerSpace = resources->virtualMemoryManager->CreateUserAddressSpace();

    process_t*        peer      = peerSpace != nullptr ? resources->processManager->CreateProcess(peerSpace) : nullptr;

    if (peer != nullptr)
    {
        peer->credentials = process->credentials;

        ok &= expect(syscall_call(SYSCALL_prlimit64, peer->id, 7, 0, output) == 0, "prlimit64 permits matching cross-process credentials");

        process_file_limit_t peerLimit = {3, original.maximum};

        fixture_store(process, base, &peerLimit, sizeof(peerLimit));

        ok &= expect(syscall_call(SYSCALL_prlimit64, peer->id, 7, base, CANONICAL_KERNEL_BASE) == LINUX_EFAULT && peer->fileLimit.current == 3 && process->fileLimit.current == original.current,
            "cross-process limit change commits before copyout fault and only changes target");

        const process_credentials_t matching = peer->credentials;

        uint32_t* mismatches[] = {&peer->credentials.user.real, &peer->credentials.user.effective, &peer->credentials.user.saved,
                                 &peer->credentials.group.real, &peer->credentials.group.effective, &peer->credentials.group.saved};

        for (size_t index = 0; index < sizeof(mismatches) / sizeof(mismatches[0]); ++index)
        {
            peer->credentials = matching;

            ++*mismatches[index];

            ok &= expect(syscall_call(SYSCALL_prlimit64, peer->id, 7, base, output) == static_cast<uint64_t>(-1) && peer->fileLimit.current == 3,
                "cross-process rlimit rejects each mismatched real/effective/saved UID and GID without mutation");
        }

        peer->credentials = matching;

        peer->exited = true;

        ProcessManager::ReleaseProcessResources(peer);

        ok &= expect(syscall_call(SYSCALL_prlimit64, peer->id, 7, 0, output) == 0, "cross-process limit query survives exited target releasing descriptor tables");

        ok &= expect(resources->processManager->FreeProcess(peer), "cross-process rlimit fixture cleanup");
    }
    else
    {
        ok &= expect(false, "cross-process rlimit fixture allocation");
    }

    resources->virtualMemoryManager->DestroyUserAddressSpace(peerSpace);

    uint64_t   now;

    const bool cpuClock = arch_time_read_monotonic(&now);

    ok &= expect(syscall_call(SYSCALL_getrlimit, 0, output) == (cpuClock ? 0 : LINUX_ENOSYS), "CPU rlimit requires an accounting clock");

    if (cpuClock)
    {
        fixture_load(process, output, &actual, sizeof(actual));

        ok &= expect(actual.current == UINT64_MAX && actual.maximum == UINT64_MAX, "CPU limits default to infinity");

        process_file_limit_t cpu = {1, 3};

        fixture_store(process, base, &cpu, sizeof(cpu));

        ok &= expect(syscall_call(SYSCALL_prlimit64, 0, 0, base, base) == 0, "CPU limit exchange permits aliased input and output");

        fixture_load(process, base, &actual, sizeof(actual));

        ok &= expect(actual.current == UINT64_MAX && process->cpuLimit.current == 1 && process->cpuLimit.maximum == 3, "CPU limit exchange returns old limit and commits enforcement state");

        cpu = {4, 3};

        fixture_store(process, base, &cpu, sizeof(cpu));

        ok &= expect(syscall_call(SYSCALL_setrlimit, 0, base) == LINUX_EINVAL, "CPU soft limit cannot exceed hard limit");

        cpu = {1, 4};

        fixture_store(process, base, &cpu, sizeof(cpu));

        ok &= expect(syscall_call(SYSCALL_setrlimit, 0, base) == static_cast<uint64_t>(-1) && process->cpuLimit.maximum == 3, "CPU hard-limit privilege override is explicitly denied");
    }

    process_file_limit_t next = {2, original.maximum};

    fixture_store(process, base, &next, sizeof(next));

    if (number == SYSCALL_prlimit64)
    {
        ok &= expect(syscall_call(number, process->id, 7, base, output) == 0, "prlimit64 current PID exchange");

        fixture_load(process, output, &actual, sizeof(actual));

        ok &= expect(actual.current == original.current && actual.maximum == original.maximum, "prlimit64 returns previous limit");
    }
    else
    {
        ok &= expect(syscall_call(SYSCALL_setrlimit, 7, base) == 0, "setrlimit lowers soft limit");
    }

    memcpy(fixture_bytes(process, base + 128), "/test.txt", sizeof("/test.txt"));

    uint64_t fd = syscall_call(SYSCALL_open, base + 128, 0);

    uint64_t duplicate = syscall_call(SYSCALL_dup, fd);

    ok &= expect(fd == 0 && duplicate == 1, "nofile permits descriptors below soft limit");

    ok &= expect(syscall_call(SYSCALL_open, base + 128, 0) == LINUX_EMFILE && syscall_call(SYSCALL_dup, fd) == LINUX_EMFILE, "nofile open and dup exhaustion");

    ok &= expect(syscall_call(SYSCALL_dup2, fd, 2) == LINUX_EBADF && syscall_call(SYSCALL_dup3, fd, 2, 0) == LINUX_EBADF, "nofile fixed duplication boundary");

    ok &= expect(syscall_call(SYSCALL_fcntl, fd, 0, 2) == LINUX_EINVAL, "nofile F_DUPFD boundary");

    next.current = 0;

    fixture_store(process, base, &next, sizeof(next));

    ok &= expect(syscall_call(SYSCALL_setrlimit, 7, base) == 0, "nofile zero soft limit");

    ok &= expect(syscall_call(SYSCALL_fcntl, duplicate, 1) == 0, "lowering limit preserves existing descriptors");

    ok &= expect(syscall_call(SYSCALL_dup2, duplicate, duplicate) == duplicate, "dup2 identity remains valid above lowered limit");

    ok &= cleanup(syscall_call(SYSCALL_close, fd) == 0 && syscall_call(SYSCALL_close, duplicate) == 0, "nofile descriptor cleanup");

    next.current = original.maximum + 1;

    fixture_store(process, base, &next, sizeof(next));

    ok &= expect(syscall_call(SYSCALL_setrlimit, 7, base) == LINUX_EINVAL, "rlimit rejects soft above hard");

    next = {1, original.maximum + 1};

    fixture_store(process, base, &next, sizeof(next));

    ok &= expect(syscall_call(SYSCALL_setrlimit, 7, base) == (uint64_t) -1, "rlimit denies hard raise without credentials");

    ok &= expect(process->fileLimit.current == 0 && process->fileLimit.maximum == original.maximum, "invalid proposed limits preserve state");

    next = {4, original.maximum};

    fixture_store(process, base, &next, sizeof(next));

    ok &= expect(syscall_call(SYSCALL_prlimit64, 0, 7, base, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "prlimit64 invalid previous output");

    ok &= expect(process->fileLimit.current == 4, "Linux prlimit64 commits before previous-output EFAULT");

    ok &= expect(syscall_call(SYSCALL_setrlimit, 7, CANONICAL_KERNEL_BASE) == LINUX_EFAULT && syscall_call(SYSCALL_getrlimit, 7, 0) == LINUX_EFAULT, "rlimit invalid buffers");

    ok &= expect(syscall_call(SYSCALL_prlimit64, 0, 16) == LINUX_EINVAL && syscall_call(SYSCALL_prlimit64, 0, 1) == LINUX_EOPNOTSUPP, "rlimit invalid and unsupported resources");

    ok &= expect(syscall_call(SYSCALL_prlimit64, UINT64_MAX, 7) == LINUX_ESRCH && syscall_call(SYSCALL_prlimit64, ProcessManager::MAX_PROCESSES, 7) == LINUX_ESRCH,
        "prlimit64 nonexistent PID restrictions");

    ok &= expect(syscall_call(SYSCALL_prlimit64, 0, 7, base, base) == 0, "prlimit64 aliased input and output");

    fixture_load(process, base, &actual, sizeof(actual));

    ok &= expect(actual.current == 4 && process->fileLimit.current == 4, "prlimit64 alias snapshots input before output");

    next = {1, 4};

    fixture_store(process, base, &next, sizeof(next));

    ok &= expect(syscall_call(SYSCALL_setrlimit, 7, base) == 0 && process->fileLimit.maximum == 4, "setrlimit lowers hard limit");

    fixture_store(process, base, &original, sizeof(original));

    ok &= expect(syscall_call(SYSCALL_setrlimit, 7, base) == (uint64_t) -1, "hard limit reduction is irreversible without privilege");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_getrlimit, 7, base) == LINUX_EFAULT, "getrlimit readonly output");

    return ok;
}

static bool test_system_info(process_t* process, uint64_t number)
{
    uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    const size_t   length                    = number == SYSCALL_uname ? 390 : 112;
    const uint64_t output                    = base + PAGE_SIZE - 32;
    *fixture_bytes(process, output - 1)      = 0xa5;
    *fixture_bytes(process, output + length) = 0xa5;
    uint64_t result                          = syscall_call(number, output);

    bool ok = expect(syscall_call(number, 0) == LINUX_EFAULT, "system info rejects null output");

    ok &= expect(result == 0, "system info cross-page output");

    ok &= expect(*fixture_bytes(process, output - 1) == 0xa5 && *fixture_bytes(process, output + length) == 0xa5, "system info writes exact ABI size");

    uint8_t data[390] = {};

    fixture_load(process, output, data, length);

    if (number == SYSCALL_uname)
    {
        ok &= expect(strcmp((char*) data, "Arx") == 0 && strcmp((char*) data + 65, "arx-kernel") == 0, "uname identity fields");

        for (size_t field = 0; field < 6; ++field)
        {
            ok &= expect(data[field * 65 + 64] == 0, "uname fields are NUL terminated");
        }

#if defined(__x86_64__)
        ok &= expect(strcmp((char*) data + 4 * 65, "x86_64") == 0, "uname target architecture");
#else
        ok &= expect(strcmp((char*) data + 4 * 65, "aarch64") == 0, "uname target architecture");
#endif
    }
    else
    {
        int64_t  uptime = 0;
        uint64_t total = 0, free = 0;
        uint16_t tasks = 0;
        uint32_t unit  = 0;

        memcpy(&uptime, data, 8);

        memcpy(&total, data + 32, 8);

        memcpy(&free, data + 40, 8);

        memcpy(&tasks, data + 80, 2);

        memcpy(&unit, data + 104, 4);

        Dispatcher*     dispatcher  = static_cast<Dispatcher*>(platform.dispacher);
        time_timespec_t now         = {};
        uint64_t        clockResult = dispatcher->GetResourceLayerCaps()->timeManager->ReadClock(1, &now);

        ok &= expect(clockResult == 0 && uptime >= 0 && uptime <= now.seconds + 1 && total >= free && unit == 1, "sysinfo uptime and memory units");

        ok &= expect(tasks > 0 && tasks <= TaskManager::MAX_TASKS, "sysinfo counts allocated tasks");

        ok &= expect(data[82] == 0 && data[83] == 0 && data[84] == 0 && data[108] == 0 && data[111] == 0, "sysinfo zeroes ABI padding");
    }

    ok &= expect(syscall_call(number, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "system info rejects kernel output");

    ok &= expect(syscall_call(number, base + 2 * PAGE_SIZE - length + 1) == LINUX_EFAULT, "system info rejects partly unmapped output");

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

    ok &= expect(syscall_call(number, output) == LINUX_EFAULT, "system info rejects readonly output page");

    return ok;
}

template <uint64_t Number>
static bool validate_system(const syscall_case_t*)
{
    return with_process([](process_t* process)
    {
        if (Number == SYSCALL_arch_prctl)
        {
            return test_system_tls(process);
        }

        if (Number == SYSCALL_prctl)
        {
            return test_system_name(process);
        }

        if (Number == SYSCALL_uname || Number == SYSCALL_sysinfo)
        {
            return test_system_info(process, Number);
        }

        if (Number == SYSCALL_getrlimit || Number == SYSCALL_setrlimit || Number == SYSCALL_prlimit64)
        {
            return test_system_limits(process, Number);
        }

        if (Number == SYSCALL_getrandom)
        {
            return expect(syscall_call(Number, 0, 0, 0) == LINUX_ENOSYS && syscall_call(Number, 0, 16, 1) == LINUX_ENOSYS && syscall_call(Number, 0, 16, 8) == LINUX_EINVAL &&
                              syscall_call(Number, 0, 16, 6) == LINUX_EINVAL,
                "getrandom rejects invalid flags and reports missing entropy subsystem");
        }

        return false;
    });
}

#if defined(__x86_64__)
static constexpr auto        native_coverage = syscall_coverage_t::Tested;
static constexpr const char* native_reason   = nullptr;
#else
static constexpr auto        native_coverage = syscall_coverage_t::Unsupported;
static constexpr const char* native_reason   = "arch_prctl is x86-64 specific";
#endif

void test_system_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<SystemRequestManager> tests[] = {
        {"HandleArch_prctlRequest", &SystemRequestManager::HandleArch_prctlRequest, SYSCALL_arch_prctl, validate_system<SYSCALL_arch_prctl>, native_coverage, native_reason},
        {"HandleUmaskRequest", &SystemRequestManager::HandleUmaskRequest, SYSCALL_umask, validate_umask},
        {"HandlePrlimit64Request", &SystemRequestManager::HandlePrlimit64Request, SYSCALL_prlimit64, validate_system<SYSCALL_prlimit64>},
        {"HandleGetrlimitRequest", &SystemRequestManager::HandleGetrlimitRequest, SYSCALL_getrlimit, validate_system<SYSCALL_getrlimit>},
        {"HandleSetrlimitRequest", &SystemRequestManager::HandleSetrlimitRequest, SYSCALL_setrlimit, validate_system<SYSCALL_setrlimit>},
        {"HandlePrctlRequest", &SystemRequestManager::HandlePrctlRequest, SYSCALL_prctl, validate_system<SYSCALL_prctl>},
        {"HandleUnameRequest", &SystemRequestManager::HandleUnameRequest, SYSCALL_uname, validate_system<SYSCALL_uname>},
        {"HandleSysinfoRequest", &SystemRequestManager::HandleSysinfoRequest, SYSCALL_sysinfo, validate_system<SYSCALL_sysinfo>},
        {"HandleGetrandomRequest", &SystemRequestManager::HandleGetrandomRequest, SYSCALL_getrandom, validate_system<SYSCALL_getrandom>, syscall_coverage_t::Unimplemented,
            "entropy subsystem is not implemented"},
    };

    if (caps->systemRequestManager != nullptr)
    {

        for (const auto& test : tests)
        {

            if ((caps->systemRequestManager->*(test.method))(nullptr) != LINUX_EINVAL)
            {
                expect(false, "system handler null-frame validation");
            }
        }
    }

    run_syscalls(caps->systemRequestManager, tests);
}

} // namespace request_selftest
