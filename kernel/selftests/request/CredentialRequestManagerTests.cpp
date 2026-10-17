#include "RequestTestSupport.hpp"

namespace request_selftest
{
static bool credential_expect_identity(bool group, uint32_t real, uint32_t effective, uint32_t saved, uint32_t filesystem)
{
    Dispatcher*               dispatcher  = static_cast<Dispatcher*>(platform.dispacher);
    process_credentials_t     credentials = {};
    uint64_t                  result      = dispatcher->GetLogicLayerCaps()->credentialManager->GetCredentials(&credentials);
    const process_identity_t& identity    = group ? credentials.group : credentials.user;

    return expect(result == 0 && identity.real == real && identity.effective == effective && identity.saved == saved && identity.filesystem == filesystem,
        "credential identity matches real/effective/saved/filesystem state");
}

static bool test_credential_identity(process_t* process, uint64_t number)
{
    const uint64_t none  = UINT32_MAX;
    const bool     group = number == SYSCALL_getgid || number == SYSCALL_getegid || number == SYSCALL_getresgid || number == SYSCALL_setgid || number == SYSCALL_setregid ||
                           number == SYSCALL_setresgid || number == SYSCALL_setfsgid;
    const uint64_t set   = group ? SYSCALL_setgid : SYSCALL_setuid;
    const uint64_t re    = group ? SYSCALL_setregid : SYSCALL_setreuid;
    const uint64_t res   = group ? SYSCALL_setresgid : SYSCALL_setresuid;
    const uint64_t fs    = group ? SYSCALL_setfsgid : SYSCALL_setfsuid;

    bool ok = expect(syscall_call(SYSCALL_getuid) == 0 && syscall_call(SYSCALL_geteuid) == 0, "credential fixture starts with controlled root IDs");

    ok &= expect(syscall_call(SYSCALL_setresgid, 200, 201, 202) == 0 && syscall_call(SYSCALL_setresuid, 100, 0, 102) == 0, "credential privileged fixture setup");

    if (number == SYSCALL_getuid || number == SYSCALL_geteuid || number == SYSCALL_getgid || number == SYSCALL_getegid)
    {
        uint64_t expected = number == SYSCALL_getuid ? 100 : number == SYSCALL_geteuid ? 0
                                                         : number == SYSCALL_getgid    ? 200
                                                                                       : 201;

        ok &= expect(syscall_call(number, UINT64_MAX) == expected, "credential scalar getter ignores unused arguments");

        ok &= expect(syscall_call(res, 11, 12, 13) == 0, "credential getter observes changed identity");

        expected = number == SYSCALL_getuid || number == SYSCALL_getgid ? 11 : 12;

        return ok && expect(syscall_call(number) == expected, "credential scalar getter returns updated value");
    }

    if (number == SYSCALL_getresuid || number == SYSCALL_getresgid)
    {
        uint64_t base = fixture_map(2);

        if ((int64_t) base < 0)
        {
            return false;
        }

        const uint64_t output                = base + PAGE_SIZE - 2;
        *fixture_bytes(process, output - 1)  = 0xa5;
        *fixture_bytes(process, output + 12) = 0xa5;
        ok &= expect(syscall_call(number, output, output + 4, output + 8) == 0, "getres IDs cross mapped-page boundary");

        uint32_t ids[3] = {};

        fixture_load(process, output, ids, sizeof(ids));

        ok &= expect(ids[0] == (group ? 200U : 100U) && ids[1] == (group ? 201U : 0U) && ids[2] == (group ? 202U : 102U), "getres IDs use 32-bit wire fields");

        ok &= expect(*fixture_bytes(process, output - 1) == 0xa5 && *fixture_bytes(process, output + 12) == 0xa5, "getres IDs preserve adjacent bytes");

        ok &= expect(syscall_call(number, output, output, output) == 0, "getres aliased outputs");

        uint32_t value = 0;

        fixture_load(process, output, &value, sizeof(value));

        ok &= expect(value == (group ? 202U : 102U), "getres aliased output receives saved ID last");

        ok &= expect(syscall_call(number, output, CANONICAL_KERNEL_BASE, output + 8) == LINUX_EFAULT, "getres invalid later output");

        fixture_load(process, output, &value, sizeof(value));

        ok &= expect(value == (group ? 200U : 100U), "getres preserves Linux earlier-write behavior on fault");

        ok &= expect(syscall_call(number, 0, output, output) == LINUX_EFAULT && syscall_call(number, base + 2 * PAGE_SIZE - 2, output, output) == LINUX_EFAULT,
            "getres null and partially unmapped output");

        syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

        ok &= expect(syscall_call(number, output, output + 4, output + 8) == LINUX_EFAULT, "getres readonly cross-page output");

        return ok;
    }

    if (number == set)
    {
        ok &= expect(syscall_call(number, (1ULL << 32) | 17) == 0, "set ID privileged transition and 32-bit argument decoding");

        ok &= credential_expect_identity(group, 17, 17, 17, 17);

        if (group)
        {
            ok &= expect(syscall_call(SYSCALL_setuid, 1000) == 0, "drop group-management privilege");
        }

        ok &= expect(syscall_call(number, 99) == (uint64_t) -1, "unprivileged set ID denied");

        ok &= credential_expect_identity(group, 17, 17, 17, 17);

        ok &= expect(syscall_call(number, none) == LINUX_EINVAL && syscall_call(number, 17) == 0, "set ID rejects reserved ID and accepts own ID");

        return ok;
    }

    if (number == re)
    {
        ok &= expect(syscall_call(number, 100, group ? 201 : 0) == 0, "setre privileged arbitrary real ID");

        ok &= expect(syscall_call(res, 100, 101, 102) == 0, "setre distinct unprivileged IDs");

        if (group)
        {
            ok &= expect(syscall_call(SYSCALL_setuid, 1000) == 0, "drop setregid privilege");
        }

        ok &= expect(syscall_call(number, none, 100) == 0, "setre effective-to-real transition");

        ok &= credential_expect_identity(group, 100, 100, 102, 100);

        ok &= expect(syscall_call(number, 102, none) == (uint64_t) -1, "setre real ID cannot use saved-only ID");

        ok &= credential_expect_identity(group, 100, 100, 102, 100);

        ok &= expect(syscall_call(number, none, 102) == 0, "setre effective ID can use saved ID");

        ok &= credential_expect_identity(group, 100, 102, 102, 102);

        ok &= expect(syscall_call(number, 102, 100) == 0, "setre swaps real and effective IDs");

        ok &= credential_expect_identity(group, 102, 100, 100, 100);

        return ok;
    }

    if (number == res)
    {
        ok &= expect(syscall_call(fs, 333) == (group ? 201U : 0U), "setres fixture overrides filesystem ID");

        ok &= expect(syscall_call(number, none, none, none) == 0, "setres sentinel no-op");

        ok &= credential_expect_identity(group, group ? 200 : 100, group ? 201 : 0, group ? 202 : 102, 333);

        ok &= expect(syscall_call(number, 100, 101, 102) == 0, "setres privileged arbitrary IDs");

        if (group)
        {
            ok &= expect(syscall_call(SYSCALL_setuid, 1000) == 0, "drop setresgid privilege");
        }

        ok &= expect(syscall_call(number, 102, 100, 101) == 0, "setres unprivileged permutation");

        ok &= credential_expect_identity(group, 102, 100, 101, 100);

        ok &= expect(syscall_call(number, none, 999, none) == (uint64_t) -1, "setres unprivileged arbitrary ID denied");

        ok &= credential_expect_identity(group, 102, 100, 101, 100);

        return ok;
    }

    if (number == fs)
    {
        ok &= expect(syscall_call(number, 333) == (group ? 201U : 0U), "setfs privileged change returns previous ID");

        ok &= credential_expect_identity(group, group ? 200 : 100, group ? 201 : 0, group ? 202 : 102, 333);

        ok &= expect(syscall_call(res, 100, 101, 102) == 0, "setfs unprivileged fixture setup");

        if (group)
        {
            ok &= expect(syscall_call(SYSCALL_setuid, 1000) == 0, "drop setfsgid privilege");
        }

        ok &= expect(syscall_call(number, 102) == 101 && syscall_call(number, 999) == 102 && syscall_call(number, UINT64_MAX) == 102,
            "setfs saved ID allowed, arbitrary and invalid IDs silently refused");

        ok &= credential_expect_identity(group, 100, 101, 102, 102);

        return ok;
    }

    return false;
}

static bool test_credential_groups(process_t* process, uint64_t number)
{
    (void) number;
    uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    const uint64_t output     = base + PAGE_SIZE - 8;
    const uint32_t input[]    = {10, 1, 10, 0, UINT32_MAX - 1};
    const uint32_t expected[] = {0, 1, 10, 10, UINT32_MAX - 1};

    fixture_store(process, base, input, sizeof(input));

    bool ok = expect(syscall_call(SYSCALL_getgroups, 0, CANONICAL_KERNEL_BASE) == 0, "getgroups initially empty and count query ignores pointer");

    ok &= expect(syscall_call(SYSCALL_setgroups, 5, base) == 0, "setgroups accepts unordered IDs and duplicates");

    ok &= expect(syscall_call(SYSCALL_getgroups, 0, CANONICAL_KERNEL_BASE) == 5 && syscall_call(SYSCALL_getgroups, 5, output) == 5, "getgroups count and cross-page output");

    uint32_t actual[5] = {};

    fixture_load(process, output, actual, sizeof(actual));

    ok &= expect(memcmp(actual, expected, sizeof(expected)) == 0, "getgroups sorted IDs preserve duplicates and 32-bit range");

    process_group_list_t* original = process->supplementaryGroups;
    uint32_t              invalid  = UINT32_MAX;

    fixture_store(process, base + 64, &invalid, sizeof(invalid));

    const size_t beforeFailures = pages_in_use();

    ok &= expect(syscall_call(SYSCALL_setgroups, 1, base + 64) == LINUX_EINVAL && syscall_call(SYSCALL_setgroups, 65537, base) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_setgroups, UINT64_MAX, base) == LINUX_EINVAL,
        "setgroups reserved ID and size boundaries");

    ok &= expect(syscall_call(SYSCALL_setgroups, 5, CANONICAL_KERNEL_BASE) == LINUX_EFAULT && syscall_call(SYSCALL_setgroups, 1, base + 2 * PAGE_SIZE - 2) == LINUX_EFAULT,
        "setgroups invalid user buffers");

    ok &= expect(process->supplementaryGroups == original, "failed setgroups preserves backing list");

    ok &= expect(pages_in_use() == beforeFailures, "failed setgroups frees staging allocations");

    ok &= expect(syscall_call(SYSCALL_getgroups, 4, output) == LINUX_EINVAL && syscall_call(SYSCALL_getgroups, UINT64_MAX, output) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_getgroups, 5, CANONICAL_KERNEL_BASE) == LINUX_EFAULT,
        "getgroups capacity and pointer validation");

    Dispatcher*           dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ProcessManager*       processes  = dispatcher->GetResourceLayerCaps()->processManager;
    VirtualMemoryManager* memory     = dispatcher->GetResourceLayerCaps()->virtualMemoryManager;
    virt_addr_space_t*    space      = memory->CreateUserAddressSpace();
    process_t*            child      = space != nullptr ? processes->CreateProcess(space) : nullptr;

    ok &= expect(child != nullptr, "credential child inheritance fixture");

    if (child != nullptr)
    {
        ok &= expect(memcmp(&child->credentials, &process->credentials, sizeof(process_credentials_t)) == 0 && child->supplementaryGroups == original && original->references == 2,
            "child inherits IDs and retains immutable group backing");

        ok &= expect(syscall_call(SYSCALL_setgroups, 0, CANONICAL_KERNEL_BASE) == 0, "setgroups zero clears and ignores pointer");

        ok &= expect(child->supplementaryGroups == original && original->references == 1 && original->ids[2] == 10, "parent group replacement leaves child list alive");

        ok &= cleanup(processes->FreeProcess(child), "credential child group cleanup");

        process_t* reused = processes->AllocateProcess();

        ok &= expect(reused != nullptr && reused->supplementaryGroups == nullptr && reused->credentials.user.effective == 0 && reused->credentials.group.real == 0,
            "credential slot reuse clears inherited state");

        if (reused != nullptr)
        {
            ok &= cleanup(processes->FreeProcess(reused), "credential reused-slot cleanup");
        }
    }

    if (space != nullptr)
    {
        memory->DestroyUserAddressSpace(space);
    }

    const uint32_t maximum = ProcessManager::MAX_SUPPLEMENTARY_GROUPS;
    uint64_t       large   = fixture_map(maximum * sizeof(uint32_t) / PAGE_SIZE);

    if ((int64_t) large >= 0)
    {
        for (uint32_t offset = 0; offset < maximum; offset += PAGE_SIZE / sizeof(uint32_t))
        {
            uint32_t* page = reinterpret_cast<uint32_t*>(fixture_bytes(process, large + offset * sizeof(uint32_t)));

            for (uint32_t i = 0; i < PAGE_SIZE / sizeof(uint32_t); ++i)
            {
                page[i] = maximum - offset - i;
            }
        }

        ok &= expect(syscall_call(SYSCALL_setgroups, maximum, large) == 0 && syscall_call(SYSCALL_getgroups, maximum, large) == maximum,
            "group list supports full 65536-entry Linux bound");

        uint32_t first = 0, last = 0;

        fixture_load(process, large, &first, sizeof(first));

        fixture_load(process, large + (maximum - 1) * sizeof(uint32_t), &last, sizeof(last));

        ok &= expect(first == 1 && last == maximum, "large group list sorts descending input");
    }
    else
    {
        ok &= expect(false, "large credential group fixture mapping");
    }

    ok &= expect(syscall_call(SYSCALL_setgroups, 5, base) == 0, "restore small group fixture");

    syscall_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_getgroups, 5, output) == LINUX_EFAULT, "getgroups readonly output");

    ok &= expect(syscall_call(SYSCALL_setuid, 1000) == 0 && syscall_call(SYSCALL_setgroups, 0, 0) == (uint64_t) -1 &&
                     syscall_call(SYSCALL_setgroups, UINT64_MAX, CANONICAL_KERNEL_BASE) == (uint64_t) -1,
        "setgroups checks privilege before size or pointers");

    ok &= expect(syscall_call(SYSCALL_getgroups, 0) == 5, "unprivileged group reads remain available");

    return ok;
}

template <uint64_t Number>
static bool validate_credential(const syscall_case_t*)
{
    return with_process([](process_t* process)
    {
        Dispatcher*     dispatcher = static_cast<Dispatcher*>(platform.dispacher);
        ProcessManager* processes  = dispatcher->GetResourceLayerCaps()->processManager;
        process_t*      locked     = processes->AcquireCurrentCredentials();

        if (locked == nullptr)
        {
            return expect(false, "credential isolated fixture state");
        }

        // Only the disposable fixture is privileged. The harness's credentials
        // are retained separately and restored after the suite.
        locked->credentials = {};

        processes->ReleaseCredentials(locked);

        if (syscall_call(SYSCALL_setgroups, 0, 0) != 0)
        {
            return false;
        }

        return Number == SYSCALL_getgroups || Number == SYSCALL_setgroups ? test_credential_groups(process, Number) : test_credential_identity(process, Number);
    });
}

void test_credential_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<CredentialRequestManager> tests[] = {
        {"HandleGetuidRequest", &CredentialRequestManager::HandleGetuidRequest, SYSCALL_getuid, validate_credential<SYSCALL_getuid>},
        {"HandleGetgidRequest", &CredentialRequestManager::HandleGetgidRequest, SYSCALL_getgid, validate_credential<SYSCALL_getgid>},
        {"HandleGeteuidRequest", &CredentialRequestManager::HandleGeteuidRequest, SYSCALL_geteuid, validate_credential<SYSCALL_geteuid>},
        {"HandleGetegidRequest", &CredentialRequestManager::HandleGetegidRequest, SYSCALL_getegid, validate_credential<SYSCALL_getegid>},
        {"HandleGetresuidRequest", &CredentialRequestManager::HandleGetresuidRequest, SYSCALL_getresuid, validate_credential<SYSCALL_getresuid>},
        {"HandleGetresgidRequest", &CredentialRequestManager::HandleGetresgidRequest, SYSCALL_getresgid, validate_credential<SYSCALL_getresgid>},
        {"HandleSetuidRequest", &CredentialRequestManager::HandleSetuidRequest, SYSCALL_setuid, validate_credential<SYSCALL_setuid>},
        {"HandleSetgidRequest", &CredentialRequestManager::HandleSetgidRequest, SYSCALL_setgid, validate_credential<SYSCALL_setgid>},
        {"HandleSetreuidRequest", &CredentialRequestManager::HandleSetreuidRequest, SYSCALL_setreuid, validate_credential<SYSCALL_setreuid>},
        {"HandleSetregidRequest", &CredentialRequestManager::HandleSetregidRequest, SYSCALL_setregid, validate_credential<SYSCALL_setregid>},
        {"HandleSetresuidRequest", &CredentialRequestManager::HandleSetresuidRequest, SYSCALL_setresuid, validate_credential<SYSCALL_setresuid>},
        {"HandleSetresgidRequest", &CredentialRequestManager::HandleSetresgidRequest, SYSCALL_setresgid, validate_credential<SYSCALL_setresgid>},
        {"HandleSetfsuidRequest", &CredentialRequestManager::HandleSetfsuidRequest, SYSCALL_setfsuid, validate_credential<SYSCALL_setfsuid>},
        {"HandleSetfsgidRequest", &CredentialRequestManager::HandleSetfsgidRequest, SYSCALL_setfsgid, validate_credential<SYSCALL_setfsgid>},
        {"HandleGetgroupsRequest", &CredentialRequestManager::HandleGetgroupsRequest, SYSCALL_getgroups, validate_credential<SYSCALL_getgroups>},
        {"HandleSetgroupsRequest", &CredentialRequestManager::HandleSetgroupsRequest, SYSCALL_setgroups, validate_credential<SYSCALL_setgroups>},
    };

    Dispatcher*     dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ProcessManager* processes  = dispatcher->GetResourceLayerCaps()->processManager;
    process_t*      process    = processes->AcquireCurrentCredentials();

    if (process == nullptr)
    {
        expect(false, "credential selftests require harness process");

        return;
    }

    const process_credentials_t original       = process->credentials;
    process_group_list_t*       originalGroups = process->supplementaryGroups;

    if (originalGroups != nullptr)
    {
        __atomic_add_fetch(&originalGroups->references, 1U, __ATOMIC_RELAXED);
    }

    processes->ReleaseCredentials(process);

    if (caps->credentialRequestManager != nullptr)
    {

        for (const auto& test : tests)
        {

            if ((caps->credentialRequestManager->*(test.method))(nullptr) != LINUX_EINVAL)
            {
                expect(false, "credential handler null-frame validation");
            }
        }
    }

    CredentialRequestManager unavailable(nullptr, nullptr);

    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};

        frame.syscall_number = test.syscallNumber;

        if ((unavailable.*(test.method))(&frame) != LINUX_ENOSYS)
        {
            expect(false, "credential missing dependency handling");
        }
    }

    run_syscalls(caps->credentialRequestManager, tests);

    process = processes->AcquireCurrentCredentials();

    if (process != nullptr)
    {
        process_group_list_t* discarded = process->supplementaryGroups;

        process->credentials         = original;
        process->supplementaryGroups = originalGroups;

        processes->ReleaseCredentials(process);

        processes->ReleaseGroupList(discarded);
    }
    else
    {
        processes->ReleaseGroupList(originalGroups);

        require_cleanup(false, "credential harness state restoration");
    }
}

} // namespace request_selftest
