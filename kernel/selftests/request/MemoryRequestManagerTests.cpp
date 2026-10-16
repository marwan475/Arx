#include "RequestTestSupport.hpp"

namespace request_selftest
{
static bool test_memory_lifetime(process_t* process)
{
    auto       processManager = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->processManager;
    auto       space          = process->addressSpace;
    bool       ok             = true;
    process_t* timerProcess   = processManager->AcquireProcess(process->id, true);

    ok &= expect(timerProcess == nullptr, "timer scan skips process without armed timer");

    processManager->ReleaseProcess(timerProcess);

    process_t* pinned = processManager->AcquireCurrentMemoryProcess();

    ok &= expect(pinned == process, "memory syscall fixture pins current process");

    if (pinned != nullptr)
    {
        ok &= expect(!processManager->FreeProcess(process), "memory syscall lifetime pin blocks teardown");

        process_t* blockedAlias = processManager->CreateProcess(space);

        ok &= expect(blockedAlias == nullptr, "memory syscall pin blocks shared-root attachment");

        if (blockedAlias != nullptr)
        {
            ok &= cleanup(processManager->FreeProcess(blockedAlias), "unexpected shared-root attachment cleanup");
        }

        processManager->ReleaseMemoryProcess(pinned);
    }

    // Timer scans retain process metadata without accessing its mappings.
    // Hold that same pin to reproduce concurrent alias creation reliably.
    process_t* retained = processManager->AcquireProcess(process->id);

    ok &= expect(retained == process, "memory syscall fixture retains process metadata");

    process_t* alias = processManager->CreateProcess(space);

    processManager->ReleaseProcess(retained);

    ok &= expect(alias != nullptr, "memory syscall shared-root fixture creates alias");

    if (alias != nullptr)
    {
        process_t* exclusive = processManager->AcquireCurrentMemoryProcess();

        ok &= expect(exclusive == nullptr, "exclusive memory pin rejects shared roots");

        processManager->ReleaseMemoryProcess(exclusive);

        const uint64_t shared = fixture_map(1);

        ok &= expect((int64_t) shared >= 0 && fixture_region(alias, shared) != nullptr, "memory syscall mapping is visible through shared root");

        if ((int64_t) shared >= 0)
        {
            ok &= cleanup(syscall_call(SYSCALL_munmap, shared, PAGE_SIZE) == 0, "shared-root mapping cleanup");
        }

        ok &= expect(syscall_call(SYSCALL_brk) == process->breakBase, "brk query remains current on shared root");

        process_t* retainedAlias = processManager->AcquireProcess(alias->id);

        ok &= expect(retainedAlias == alias && !processManager->FreeProcess(alias), "metadata lifetime pin blocks process teardown");

        processManager->ReleaseProcess(retainedAlias);

        ok &= cleanup(processManager->FreeProcess(alias), "memory syscall shared-root fixture cleanup");
    }

    return ok;
}

static uint64_t memory_pte(process_t* process, uint64_t address)
{
#if defined(__x86_64__)
    uint64_t table = process->addressSpace->pt;

    for (int shift = 39; shift >= 12; shift -= 9)
    {
        uint64_t* entries = reinterpret_cast<uint64_t*>(pa_to_hhdm(table, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset));
        uint64_t  entry   = entries[(address >> shift) & 511];

        if ((entry & 1) == 0 || shift == 12)
        {
            return entry;
        }

        table = entry & 0x000ffffffffff000ULL;
    }

#endif
    return 0;
}

static bool test_memory_mmap(process_t* process)
{
    bool     ok = true;
    uint64_t a  = fixture_map();

    ok &= expect(a == 0x10000 && fixture_region(process, a), "mmap page-aligned placement");

    if (static_cast<int64_t>(a) < 0)
    {
        return false;
    }

    for (uint64_t p = a; p < a + 3 * PAGE_SIZE; p += PAGE_SIZE)
    {

        for (unsigned i = 0; i < PAGE_SIZE; ++i)
        {

            if (!fixture_bytes(process, p) || fixture_bytes(process, p)[i])
            {
                return expect(false, "mmap zero initialization");
            }
        }
    }

    uint64_t b = fixture_map(1);

    ok &= expect(b == a + 3 * PAGE_SIZE && fixture_bytes(process, b) != fixture_bytes(process, a), "mmap independent backing");

    uint64_t fixed = 0x400000;

    ok &= expect(fixture_map(1, fixed, 0x100022) == fixed, "mmap fixed noreplace");

    ok &= expect(fixture_map(1, fixed, 0x100022) == static_cast<uint64_t>(-17), "mmap collision EEXIST");

    if (fixture_bytes(process, fixed))
    {
        *fixture_bytes(process, fixed) = 0xA5;
    }

    ok &= expect(fixture_map(1, fixed, 0x32) == fixed && fixture_bytes(process, fixed) && *fixture_bytes(process, fixed) == 0, "mmap fixed replacement zeroes");

    ok &= expect(fixture_map(1, 0x600123) == 0x600000, "mmap address hint");

    ok &= expect(fixture_map(1, fixed) != fixed, "mmap occupied hint fallback");

    ok &= expect(syscall_call(SYSCALL_mmap, 0, 0, 3, 0x22) == LINUX_EINVAL, "mmap zero length");

    ok &= expect(syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 8, 0x22) == LINUX_EINVAL, "mmap invalid protection");

    ok &= expect(fixture_map(1, 0, 0x20) == LINUX_EINVAL && fixture_map(1, 0, 0x80000022) == LINUX_EINVAL, "mmap invalid flags");

    ok &= expect(syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 3, 0x22, 0, 1) == LINUX_EINVAL, "mmap offset alignment");

    ok &= expect(fixture_map(1, fixed + 1, 0x32) == LINUX_EINVAL, "mmap fixed alignment");

    ok &= expect(syscall_call(SYSCALL_mmap, CANONICAL_KERNEL_BASE, PAGE_SIZE, 3, 0x32) == LINUX_EINVAL, "mmap rejects kernel address");

    ok &= expect(syscall_call(SYSCALL_mmap, 0, UINT64_MAX, 3, 0x22) == LINUX_EINVAL, "mmap length overflow");

    const uint64_t shared = fixture_map(1, 0, 0x21);

    ok &= expect(static_cast<int64_t>(shared) >= 0 && fixture_region(process, shared)->shared_mapping, "mmap shared anonymous backing");

    if (static_cast<int64_t>(shared) >= 0)
    {
        ok &= expect(syscall_call(SYSCALL_munmap, shared, PAGE_SIZE) == 0, "shared anonymous unmap");
    }

    ok &= expect(syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 1, 2, UINT64_MAX) == LINUX_EBADF, "mmap invalid fd");

    memcpy(fixture_bytes(process, a), "/test.txt", sizeof("/test.txt"));

    uint64_t fd = syscall_call(SYSCALL_open, a, 0);

    ok &= expect(static_cast<int64_t>(fd) >= 0, "mmap file fixture opens through real VFS");

    if (static_cast<int64_t>(fd) >= 0)
    {
        const uint64_t mapped = syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 1, 2, fd);

        ok &= expect(static_cast<int64_t>(mapped) >= 0, "mmap file through VFS backing callback");

        ok &= expect(syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 3, 1, fd) == static_cast<uint64_t>(-13), "shared writable mmap requires writable descriptor");

        const uint64_t readonlyShared = syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 1, 1, fd);

        ok &= expect(static_cast<int64_t>(readonlyShared) >= 0 && syscall_call(SYSCALL_mprotect, readonlyShared, PAGE_SIZE, 3) == static_cast<uint64_t>(-13),
            "read-only descriptor mapping cannot gain shared write access");

        if (static_cast<int64_t>(readonlyShared) >= 0)
        {
            syscall_call(SYSCALL_munmap, readonlyShared, PAGE_SIZE);
        }

        // musl first maps the whole library span, including pages beyond
        // EOF, then replaces BSS with MAP_FIXED anonymous memory.
        const uint64_t beyond = syscall_call(SYSCALL_mmap, 0, 3 * PAGE_SIZE, 3, 2, fd);
        ok &= expect(static_cast<int64_t>(beyond) >= 0, "private file mmap reserves beyond EOF");

        if (static_cast<int64_t>(beyond) >= 0)
        {
            virt_region_t* tail = fixture_region(process, beyond + 2 * PAGE_SIZE);
            ok &= expect(tail != nullptr && tail->object != nullptr && tail->backing == nullptr
                         && fixture_vmm()->VirtToPhys(beyond + 2 * PAGE_SIZE, process->addressSpace) == 0, "EOF reservation consumes no resident page");
            ok &= expect(syscall_call(SYSCALL_mprotect, beyond + 2 * PAGE_SIZE, PAGE_SIZE, 3) == 0
                         && fixture_vmm()->VirtToPhys(beyond + 2 * PAGE_SIZE, process->addressSpace) == 0, "mprotect keeps EOF reservation inaccessible");
            auto* clone = fixture_vmm()->CloneUserAddressSpace(process->addressSpace);
            ok &= expect(clone != nullptr, "fork preserves nonresident file reservations");

            if (clone != nullptr)
            {
                auto* clonedTail = fixture_vmm()->FindRegion(clone, beyond + 2 * PAGE_SIZE);
                ok &= expect(clonedTail != nullptr && clonedTail->object == tail->object && clonedTail->backing == nullptr, "fork retains EOF backing ownership without allocating bytes");
                fixture_vmm()->DestroyUserAddressSpace(clone);
            }

            ok &= expect(syscall_call(SYSCALL_mmap, beyond + PAGE_SIZE, 2 * PAGE_SIZE, 3, 0x32, UINT64_MAX) == beyond + PAGE_SIZE
                         && fixture_bytes(process, beyond + PAGE_SIZE) != nullptr && *fixture_bytes(process, beyond + PAGE_SIZE) == 0, "anonymous BSS replaces EOF reservation");
            ok &= expect(syscall_call(SYSCALL_munmap, beyond, 3 * PAGE_SIZE) == 0, "unmap releases file span and replaced BSS");
        }

        ok &= expect(syscall_call(SYSCALL_close, fd) == 0, "mmap file fixture closes");

        if (static_cast<int64_t>(mapped) >= 0)
        {
            ok &= expect(fixture_region(process, mapped)->object->file && fixture_bytes(process, mapped) != nullptr, "file mapping survives descriptor closure");

            ok &= expect(syscall_call(SYSCALL_munmap, mapped, PAGE_SIZE) == 0, "file mapping unmap");
        }
    }

    // Constrain the free interval to exercise address-space exhaustion safely.
    uint64_t       failStart = 0x1000000;
    virt_region_t* free      = process->addressSpace->user_free_regions;

    while (free && !(free->start <= failStart && free->end > failStart))
    {
        free = free->next;
    }

    if (!free)
    {
        return expect(false, "mmap rollback fixture range");
    }

    uint64_t savedEnd = free->end;

    free->end  = failStart + 2 * PAGE_SIZE;
    free->size = free->end - free->start;

    ok &= expect(fixture_map(3, failStart, 0x32) == static_cast<uint64_t>(-12), "mmap allocation failure");

    ok &= expect(!fixture_region(process, failStart) && !fixture_vmm()->VirtToPhys(failStart, process->addressSpace), "mmap placement failure preserves mappings");

    // Free-list coalescing may replace the original node.

    for (virt_region_t* r = process->addressSpace->user_free_regions; r; r = r->next)
    {

        if (r->end == failStart + 2 * PAGE_SIZE)
        {
            r->end  = savedEnd;
            r->size = r->end - r->start;

            break;
        }
    }

    return ok;
}

static bool test_memory_munmap(process_t* process)
{
    uint64_t a = fixture_map();

    if (static_cast<int64_t>(a) < 0)
    {
        return false;
    }

    const size_t beforeUnmap = pages_in_use();

    bool ok = expect(syscall_call(SYSCALL_munmap, a + PAGE_SIZE, 1) == 0, "munmap middle page");

    ok &= expect(pages_in_use() + 1 == beforeUnmap, "munmap releases physical middle page");

    ok &= expect(fixture_region(process, a) && !fixture_region(process, a + PAGE_SIZE) && fixture_region(process, a + 2 * PAGE_SIZE), "munmap neighboring metadata intact");

    ok &= expect(!fixture_vmm()->VirtToPhys(a + PAGE_SIZE, process->addressSpace), "munmap leaf cleared");

    ok &= expect(syscall_call(SYSCALL_munmap, a, 3 * PAGE_SIZE) == 0 && !fixture_region(process, a) && !fixture_region(process, a + 2 * PAGE_SIZE),
        "munmap spanning regions and hole");

    ok &= expect(pages_in_use() + 3 == beforeUnmap, "munmap releases all physical backing");

    ok &= expect(syscall_call(SYSCALL_munmap, a, 3 * PAGE_SIZE) == 0, "munmap already unmapped");

    ok &= expect(fixture_map(3, a, 0x100022) == a, "munmap reservation reusable");

    ok &= expect(syscall_call(SYSCALL_munmap, a + 1, PAGE_SIZE) == LINUX_EINVAL && syscall_call(SYSCALL_munmap, a, 0) == LINUX_EINVAL, "munmap invalid alignment and length");

    return ok;
}

static bool test_memory_mprotect(process_t* process)
{
    uint64_t a = fixture_map();

    if (static_cast<int64_t>(a) < 0)
    {
        return false;
    }

    bool ok = expect(syscall_call(SYSCALL_mprotect, a + PAGE_SIZE, PAGE_SIZE, 1) == 0, "mprotect read-only subrange");

    ok &= expect(fixture_region(process, a)->protection == 3 && fixture_region(process, a + PAGE_SIZE)->protection == 1 && !(memory_pte(process, a + PAGE_SIZE) & 2),
        "mprotect metadata and readonly PTE");

    ok &= expect(syscall_call(SYSCALL_mprotect, a, PAGE_SIZE, 5) == 0 && !(memory_pte(process, a) & (1ULL << 63)), "mprotect executable PTE");

    ok &= expect(syscall_call(SYSCALL_mprotect, a, PAGE_SIZE, 0) == 0 && !(memory_pte(process, a) & 4) && fixture_region(process, a)->backing,
        "mprotect none denies user access retains backing");

    ok &= expect(syscall_call(SYSCALL_mprotect, a, PAGE_SIZE, 3) == 0 && (memory_pte(process, a) & 6) == 6 && (memory_pte(process, a) & (1ULL << 63)),
        "mprotect restores write and NX");

    for (uint32_t protection = 0; protection < 8; ++protection)
    {
        ok &= expect(syscall_call(SYSCALL_mprotect, a + 2 * PAGE_SIZE, PAGE_SIZE, protection) == 0 && fixture_region(process, a + 2 * PAGE_SIZE)->protection == protection,
            "mprotect all valid protection combinations");
    }

    ok &= expect(syscall_call(SYSCALL_mprotect, a, PAGE_SIZE, 8) == LINUX_EINVAL && syscall_call(SYSCALL_mprotect, a + 1, PAGE_SIZE, 1) == LINUX_EINVAL,
        "mprotect invalid flags/alignment");

    ok &= expect(syscall_call(SYSCALL_mprotect, 0x400000, PAGE_SIZE, 1) == static_cast<uint64_t>(-12), "mprotect unmapped ENOMEM");

    ok &= expect(syscall_call(SYSCALL_mprotect, CANONICAL_KERNEL_BASE, PAGE_SIZE, 1) == LINUX_EINVAL, "mprotect kernel address rejected");

    void* legacyPage = pmm_alloc(PAGE_SIZE);

    if (legacyPage == nullptr)
    {
        return expect(false, "mprotect legacy mapping fixture allocation");
    }

    const phys_addr_t legacyPhysical = hhdm_to_pa(reinterpret_cast<uintptr_t>(legacyPage), platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
    uint64_t          legacyFlags    = 0;

    ARCH_PAGE_FLAGS_INIT(legacyFlags);

    ARCH_PAGE_FLAG_SET_READ(legacyFlags);

    ARCH_PAGE_FLAG_SET_WRITE(legacyFlags);

    ARCH_PAGE_FLAG_SET_USER(legacyFlags);

    fixture_vmm()->MapPage(0x400000, legacyPhysical, legacyFlags, process->addressSpace);

    ok &= expect(fixture_vmm()->VirtToPhys(0x400000, process->addressSpace) == legacyPhysical, "mprotect legacy mapping fixture maps");

    ok &= expect(syscall_call(SYSCALL_mprotect, 0x400000, PAGE_SIZE, 1) == static_cast<uint64_t>(-95), "mprotect rejects untracked backing");

    ok &= expect(syscall_call(SYSCALL_munmap, 0x400000, PAGE_SIZE) == static_cast<uint64_t>(-95), "munmap preserves untracked backing");

    ok &= expect(fixture_map(1, 0x400000, 0x32) == static_cast<uint64_t>(-95), "mmap fixed preserves untracked backing");

    ok &= expect(syscall_call(SYSCALL_madvise, 0x400000, PAGE_SIZE, 4) == static_cast<uint64_t>(-95), "madvise rejects untracked discard");

    fixture_vmm()->UnmapPage(0x400000, process->addressSpace);

    pmm_free(legacyPage);

    return ok;
}

static bool test_memory_mincore(process_t* process)
{
    uint64_t a = fixture_map(), vec = fixture_map(2);

    if (static_cast<int64_t>(a) < 0 || static_cast<int64_t>(vec) < 0)
    {
        return false;
    }

    *fixture_bytes(process, vec + PAGE_SIZE - 2) = 0xA5;
    *fixture_bytes(process, vec + PAGE_SIZE + 2) = 0xA5;
    syscall_call(SYSCALL_mprotect, a, PAGE_SIZE, 0);

    bool ok = expect(syscall_call(SYSCALL_mincore, a, 2 * PAGE_SIZE + 1, vec + PAGE_SIZE - 1) == 0, "mincore multiple pages cross-buffer page");

    ok &= expect(*fixture_bytes(process, vec + PAGE_SIZE - 1) == 1 && *fixture_bytes(process, vec + PAGE_SIZE) == 1 && *fixture_bytes(process, vec + PAGE_SIZE + 1) == 1,
        "mincore residency including PROT_NONE");

    ok &= expect(*fixture_bytes(process, vec + PAGE_SIZE - 2) == 0xA5 && *fixture_bytes(process, vec + PAGE_SIZE + 2) == 0xA5, "mincore exact vector length");

    ok &= expect(syscall_call(SYSCALL_mincore, a + 1, PAGE_SIZE, vec) == LINUX_EINVAL, "mincore alignment");

    ok &= expect(syscall_call(SYSCALL_mincore, a, PAGE_SIZE, reinterpret_cast<uint64_t>(&process)) == LINUX_EFAULT, "mincore rejects kernel output");

    ok &= expect(syscall_call(SYSCALL_mincore, a, PAGE_SIZE, 0x400000) == LINUX_EFAULT, "mincore unmapped output");

    ok &= expect(syscall_call(SYSCALL_mincore, 0x400000, PAGE_SIZE, vec) == static_cast<uint64_t>(-12), "mincore unmapped source");

    ok &= expect(syscall_call(SYSCALL_mincore, a, 0, 0) == 0, "mincore zero-length query");

    ok &= expect(syscall_call(SYSCALL_mincore, a, 3 * PAGE_SIZE, vec + 2 * PAGE_SIZE - 1) == LINUX_EFAULT, "mincore buffer crosses unmapped page");

    syscall_call(SYSCALL_mprotect, vec, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_mincore, a, PAGE_SIZE, vec) == LINUX_EFAULT, "mincore readonly output rejected");

    return ok;
}

static bool test_memory_madvise(process_t* process)
{
    uint64_t a = fixture_map();

    if (static_cast<int64_t>(a) < 0)
    {
        return false;
    }

    bool ok                    = true;
    *fixture_bytes(process, a) = 0xA5;

    for (int advice = 0; advice <= 3; ++advice)
    {
        ok &= expect(syscall_call(SYSCALL_madvise, a, PAGE_SIZE, advice) == 0 && *fixture_bytes(process, a) == 0xA5, "madvise hint accepted preserves content");
    }

    ok &= expect(syscall_call(SYSCALL_madvise, a, PAGE_SIZE, 4) == 0 && *fixture_bytes(process, a) == 0, "madvise dontneed discards contents");

    ok &= expect(syscall_call(SYSCALL_madvise, a + 1, PAGE_SIZE, 0) == LINUX_EINVAL, "madvise alignment");

    ok &= expect(syscall_call(SYSCALL_madvise, a, PAGE_SIZE, 999) == LINUX_EINVAL && syscall_call(SYSCALL_madvise, a, PAGE_SIZE, 8) == LINUX_EINVAL,
        "madvise invalid and unsupported FREE");

    ok &= expect(syscall_call(SYSCALL_madvise, 0x400000, PAGE_SIZE, 0) == static_cast<uint64_t>(-12), "madvise unmapped range");

    syscall_call(SYSCALL_munmap, a + PAGE_SIZE, PAGE_SIZE);
    *fixture_bytes(process, a)                 = 0xA5;
    *fixture_bytes(process, a + 2 * PAGE_SIZE) = 0xA5;
    ok &= expect(syscall_call(SYSCALL_madvise, a, 3 * PAGE_SIZE, 4) == static_cast<uint64_t>(-12) && *fixture_bytes(process, a) == 0 &&
                     *fixture_bytes(process, a + 2 * PAGE_SIZE) == 0,
        "madvise holes return ENOMEM and process mapped pages");

    return ok;
}

static bool test_memory_brk(process_t* process)
{
    uint64_t base = process->breakBase;

    bool ok = expect(syscall_call(SYSCALL_brk) == base, "brk initial query");

    ok &= expect(syscall_call(SYSCALL_brk, base + 1) == base + 1 && fixture_bytes(process, base) && *fixture_bytes(process, base) == 0, "brk grows zeroed page");

    if (!fixture_bytes(process, base))
    {
        return false;
    }

    *fixture_bytes(process, base) = 0xA5;
    ok &= expect(syscall_call(SYSCALL_brk, base + 2 * PAGE_SIZE + 17) == base + 2 * PAGE_SIZE + 17 && *fixture_bytes(process, base) == 0xA5 &&
                     fixture_bytes(process, base + 2 * PAGE_SIZE) && *fixture_bytes(process, base + 2 * PAGE_SIZE) == 0,
        "brk page boundary and contents");

    ok &= expect(syscall_call(SYSCALL_brk, base + 7) == base + 7 && !fixture_region(process, base + PAGE_SIZE) && fixture_region(process, base), "brk shrink releases whole pages");

    ok &= expect(syscall_call(SYSCALL_brk) == base + 7, "brk resized query");
    *fixture_bytes(process, base + 10) = 0x5A;
    ok &= expect(syscall_call(SYSCALL_brk, base + 11) == base + 11 && *fixture_bytes(process, base + 10) == 0x5A, "brk retained partial page regrowth");

    ok &= expect(syscall_call(SYSCALL_brk, base + 7) == base + 7, "brk restores small break");

    ok &= expect(fixture_map(1, base + PAGE_SIZE, 0x100022) == base + PAGE_SIZE, "brk collision fixture");

    ok &= expect(syscall_call(SYSCALL_brk, base + 2 * PAGE_SIZE) == base + 7, "brk collision returns current break");

    ok &= expect(syscall_call(SYSCALL_brk, base - 1) == base + 7 && syscall_call(SYSCALL_brk, CANONICAL_KERNEL_BASE) == base + 7, "brk invalid requests return current break");

    ok &= expect(syscall_call(SYSCALL_brk, base) == base && !fixture_region(process, base), "brk full shrink");

    return ok;
}

static bool validate_mmap(const syscall_case_t*)
{
    return REQUEST_EXPECT(with_process(test_memory_mmap));
}

static bool validate_munmap(const syscall_case_t*)
{
    return REQUEST_EXPECT(with_process(test_memory_munmap));
}

static bool validate_mprotect(const syscall_case_t*)
{
    return REQUEST_EXPECT(with_process(test_memory_mprotect));
}

static bool validate_mincore(const syscall_case_t*)
{
    return REQUEST_EXPECT(with_process(test_memory_mincore));
}

static bool validate_madvise(const syscall_case_t*)
{
    return REQUEST_EXPECT(with_process(test_memory_madvise));
}

static bool validate_brk(const syscall_case_t*)
{
    return REQUEST_EXPECT(with_process(test_memory_brk));
}

void test_memory_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<MemoryRequestManager> tests[] = {
        {"HandleMmapRequest", &MemoryRequestManager::HandleMmapRequest, SYSCALL_mmap, validate_mmap},
        {"HandleMunmapRequest", &MemoryRequestManager::HandleMunmapRequest, SYSCALL_munmap, validate_munmap},
        {"HandleMprotectRequest", &MemoryRequestManager::HandleMprotectRequest, SYSCALL_mprotect, validate_mprotect},
        {"HandleMincoreRequest", &MemoryRequestManager::HandleMincoreRequest, SYSCALL_mincore, validate_mincore},
        {"HandleMadviseRequest", &MemoryRequestManager::HandleMadviseRequest, SYSCALL_madvise, validate_madvise},
        {"HandleBrkRequest", &MemoryRequestManager::HandleBrkRequest, SYSCALL_brk, validate_brk},
    };

    expect(with_process(test_memory_lifetime), "memory process lifetime contract");

    run_syscalls(caps->memoryRequestManager, tests);
}

} // namespace request_selftest
