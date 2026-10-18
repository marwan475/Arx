#include "RequestTestSupport.hpp"

namespace request_selftest
{
request_user_fixture_t* request_user_fixtures[BOOT_SMP_MAX_CPUS];
SuiteContext*           active_suite;

SuiteContext::SuiteContext(TestCase& test, const char* manager) : test(test), manager(manager), syscall("fixture")
{
    active_suite = this;
}

SuiteContext::~SuiteContext()
{
    if (passed != 0 || failed != 0 || skipped != 0)
    {
        kprintf("[SELFTEST] request/%s syscalls(pass=%u fail=%u skip=%u)\n", manager, passed, failed, skipped);
    }

    if (passed == 0 && failed == 0 && skipped != 0)
    {
        test.Skip("all syscall contracts have explicit coverage gaps");
    }

    active_suite = nullptr;
}

void SuiteContext::Begin(const char* name)
{
    syscall        = name;
    current_failed = false;

    kprintf("[SELFTEST] RUN request/%s/%s\n", manager, syscall);
}

void SuiteContext::Finish(bool success)
{
    success &= !current_failed;

    success ? ++passed : ++failed;

    kprintf("[SELFTEST] %s request/%s/%s\n", success ? "PASS" : "FAIL", manager, syscall);

    syscall = "fixture";
}

void SuiteContext::Skip(const char* reason)
{
    if (current_failed)
    {
        Finish(false);

        return;
    }

    ++skipped;

    kprintf("[SELFTEST] SKIP request/%s/%s: %s\n", manager, syscall, reason);

    syscall = "fixture";
}

bool expect(bool condition, const char* expectation)
{
    active_suite->current_failed |= !condition;

    char detail[256];

    snprintf(detail, sizeof(detail), "%s/%s: %s", active_suite->manager, active_suite->syscall, expectation);

    return active_suite->test.Check(condition, detail);
}

bool equal(uint64_t actual, uint64_t expected, const char* expectation)
{
    active_suite->current_failed |= actual != expected;

    char detail[256];

    snprintf(detail, sizeof(detail), "%s/%s: %s", active_suite->manager, active_suite->syscall, expectation);

    return active_suite->test.Equal(actual, expected, detail);
}

void require_cleanup(bool condition, const char* expectation)
{
    if (!expect(condition, expectation))
    {
        SelfTestManager::Get().AbortUnsafe("request fixture cleanup failed; later tests cannot safely run");
    }
}

bool cleanup(bool condition, const char* expectation)
{
    require_cleanup(condition, expectation);

    return true;
}

uint64_t coordination_deadline()
{
    uint64_t now = 0;

    if (!expect(arch_time_read_monotonic(&now), "peer coordination requires a monotonic clock"))
    {
        SelfTestManager::Get().AbortUnsafe("cannot bound request peer coordination without a clock");
    }

    return now + 2000000000ULL;
}

ProcessFixture::ProcessFixture()
{
    cpu_preemption_disable();

    auto dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto caps       = dispatcher != nullptr ? dispatcher->GetResourceLayerCaps() : nullptr;

    if (!expect(caps != nullptr && caps->processManager != nullptr && caps->virtualMemoryManager != nullptr, "isolated process requires resource managers"))
    {
        return;
    }

    processes = caps->processManager;
    memory    = caps->virtualMemoryManager;
    previous  = processes->GetCurrentProcess();

    if (!expect(previous != nullptr && previous->addressSpace != nullptr, "isolated process requires restorable caller"))
    {
        return;
    }

    space = memory->CreateUserAddressSpace();

    if (!expect(space != nullptr, "isolated address space allocation"))
    {
        return;
    }

    process = processes->CreateProcess(space);

    if (!expect(process != nullptr, "isolated process allocation"))
    {
        return;
    }

    process->breakBase    = 0x800000;
    process->programBreak = process->breakBase;

    ready = expect(processes->SetRunningProcess(arch_cpu_id(), process), "activate isolated process");
}

ProcessFixture::~ProcessFixture()
{
    if (ready)
    {
        require_cleanup(processes->SetRunningProcess(arch_cpu_id(), previous), "restore caller process");
    }

    if (process != nullptr)
    {
        require_cleanup(processes->FreeProcess(process), "reclaim isolated process");
    }

    if (space != nullptr)
    {
        require_cleanup(space->user_used_regions == nullptr, "isolated process releases owned pages");

        memory->DestroyUserAddressSpace(space);
    }

    cpu_preemption_enable();
}

request_user_fixture_t::request_user_fixture_t() : space(platform.cpus[arch_cpu_id()].address_space), previous(request_user_fixtures[arch_cpu_id()]), count(0), valid(true)
{
    request_user_fixtures[arch_cpu_id()] = this;
}

request_user_fixture_t::~request_user_fixture_t()
{
    cpu_preemption_disable();

    bool released = true;

    if (space != nullptr)
    {
        spinlock_acquire(&space->operation_lock);

        for (size_t i = 0; i < count; ++i)
        {
            vmm_unmap_page(pages[i], space);

            vmm_free_region(space, pages[i]);

            released &= vmm_virt_to_phys(pages[i], space) == 0;
        }

        spinlock_release(&space->operation_lock);
    }

    request_user_fixtures[arch_cpu_id()] = previous;

    cpu_preemption_enable();

    require_cleanup(released, "release all temporary user aliases");
}

uint64_t request_user_fixture_t::Address(const void* buffer, size_t length)
{
    const uint64_t source = (uint64_t) (uintptr_t) buffer;

    if (source < CANONICAL_KERNEL_BASE)
    {
        return source;
    }

    const size_t offset = source & (PAGE_SIZE - 1);

    if (space == nullptr || length == 0 || length > MAX_PAGES * PAGE_SIZE - offset)
    {
        valid = false;

        return 0;
    }

    const size_t pageCount = align_up(offset + length, PAGE_SIZE) / PAGE_SIZE;

    if (pageCount > MAX_PAGES - count)
    {
        valid = false;

        return 0;
    }

    cpu_preemption_disable();

    spinlock_acquire(&space->operation_lock);

    const uint64_t base = USER_BASE + count * PAGE_SIZE;
    uint64_t       flags;

    ARCH_PAGE_FLAGS_INIT(flags);

    ARCH_PAGE_FLAG_SET_READ(flags);

    ARCH_PAGE_FLAG_SET_WRITE(flags);

    ARCH_PAGE_FLAG_SET_USER(flags);

    bool          mapped = true;
    const zone_t* zone   = &platform.cpus[arch_cpu_id()].numa_node->zone;

    for (size_t i = 0; i < pageCount; ++i)
    {
        const uint64_t kernelPage = align_down(source, PAGE_SIZE) + i * PAGE_SIZE;
        phys_addr_t    physical   = 0;

        if (zone->hhdm_present && kernelPage >= zone->hhdm_offset && (kernelPage - zone->hhdm_offset) / PAGE_SIZE < zone->max_pfn)
        {
            physical = kernelPage - zone->hhdm_offset;
        }
        else
        {
            physical = vmm_virt_to_phys(kernelPage, space);
        }

        const uint64_t userPage = base + i * PAGE_SIZE;

        if (physical == 0 || vmm_virt_to_phys(userPage, space) != 0 || !vmm_reserve_page_at(space, userPage))
        {
            mapped = false;

            break;
        }

        pages[count++] = userPage;

        vmm_map_page(userPage, physical, flags, space);

        if (vmm_virt_to_phys(userPage, space) != physical || !arch_user_page_accessible(userPage, space->pt, true))
        {
            mapped = false;

            break;
        }
    }

    spinlock_release(&space->operation_lock);

    cpu_preemption_enable();

    valid &= mapped;

    return mapped ? base + offset : 0;
}

uint64_t request_user_address(const void* buffer, size_t length)
{
    request_user_fixture_t* fixture = request_user_fixtures[arch_cpu_id()];

    return fixture != nullptr ? fixture->Address(buffer, length) : 0;
}

VirtualMemoryManager* fixture_vmm()
{
    Dispatcher* dispatcher = static_cast<Dispatcher*>(platform.dispacher);

    return dispatcher->GetResourceLayerCaps()->virtualMemoryManager;
}

uint64_t syscall_call(uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    arch_syscall_frame_t frame = {};

    frame.syscall_number = number;
    frame.arg0           = arg0;
    frame.arg1           = arg1;
    frame.arg2           = arg2;
    frame.arg3           = arg3;
    frame.arg4           = arg4;
    frame.arg5           = arg5;

    return dispatcher_dispatch_syscall(&frame);
}

uint64_t fixture_map(uint64_t pages, uint64_t address, uint64_t flags)
{
    return syscall_call(SYSCALL_mmap, address, pages * PAGE_SIZE, 3, flags, UINT64_MAX);
}

virt_region_t* fixture_region(process_t* process, uint64_t address)
{
    return fixture_vmm()->FindRegion(process->addressSpace, address);
}

uint8_t* fixture_bytes(process_t* process, uint64_t address)
{
    virt_region_t* region = fixture_region(process, address);

    return region != nullptr && region->backing != nullptr ? static_cast<uint8_t*>(region->backing) + (address & (PAGE_SIZE - 1)) : nullptr;
}

void fixture_store(process_t* process, uint64_t address, const void* source, size_t length)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(source);

    for (size_t i = 0; i < length; ++i)
    {
        *fixture_bytes(process, address + i) = bytes[i];
    }
}

void fixture_load(process_t* process, uint64_t address, void* destination, size_t length)
{
    uint8_t* bytes = static_cast<uint8_t*>(destination);

    for (size_t i = 0; i < length; ++i)
    {
        bytes[i] = *fixture_bytes(process, address + i);
    }
}

void detach_fixture_peer(ResourceLayerCaps* resources, process_t* process, task_t* peer)
{
    resources->processManager->RemoveTask(process, peer);

    // with_process switches the running process without moving the
    // caller into its task list. The fixture still owns its default contexts
    // after its last peer exits. FreeTask must release only the peer's refs.
    peer->process = nullptr;
}

size_t descriptor_count(process_t* process)
{
    size_t count = 0;

    for (size_t i = 0; i < process->Files()->count; ++i)
    {
        count += process->Files()->entries[i].file != nullptr;
    }

    return count;
}

void syscall_peer_entry(void* argument)
{
    cpu_preemption_disable();

    auto context = static_cast<syscall_peer_context_t*>(argument);

    context->result = syscall_call(context->number, context->args[0], context->args[1], context->args[2], context->args[3], context->args[4], context->args[5]);

    context->scheduler->ExitCurrentTask();
}

size_t pages_in_use()
{
    zone_t* zone = &platform.cpus[arch_cpu_id()].numa_node->zone;

    spinlock_acquire(&zone->lock);

    const size_t usedPages = zone->used_pages;

    spinlock_release(&zone->lock);

    return usedPages;
}

void test_user_copy_fixture()
{
    request_user_fixture_t fixture;
    uint32_t               original    = 0x12345678;
    const uint32_t         replacement = 0xABCDEF01;
    uint32_t               copied      = 0;
    const uint64_t         user        = request_user_address(&original, sizeof(original));
    bool                   passed      = user != 0 && fixture.valid;

    if (passed)
    {
        passed &= safe_read_user(user, &copied, sizeof(copied)) && copied == original;

        passed &= safe_write_user(user, &replacement, sizeof(replacement)) && original == replacement;

        passed &= !safe_read_user((uint64_t) (uintptr_t) &original, &copied, sizeof(copied));

        passed &= !safe_write_user((uint64_t) (uintptr_t) &original, &copied, sizeof(copied)) && original == replacement;

        passed &= !safe_read_user(CANONICAL_USER_END, &copied, sizeof(copied));

        arch_syscall_frame_t frame = {};

        frame.syscall_number = SYSCALL_getcpu;

        // Deliberately bypass the fixture: the actual handler must reject it.
        frame.arg0 = (uint64_t) (uintptr_t) &original;

        passed &= dispatcher_dispatch_syscall(&frame) == LINUX_EFAULT && original == replacement;

        frame                = {};
        frame.syscall_number = SYSCALL_open;
        frame.arg0           = (uint64_t) (uintptr_t) "/test.txt";

        passed &= dispatcher_dispatch_syscall(&frame) == LINUX_EFAULT;

        uint64_t flags;

        ARCH_PAGE_FLAGS_INIT(flags);

        ARCH_PAGE_FLAG_SET_READ(flags);

        ARCH_PAGE_FLAG_SET_USER(flags);

        cpu_preemption_disable();

        vmm_protect_page(align_down(user, PAGE_SIZE), flags, fixture.space);

        cpu_preemption_enable();

        passed &= !safe_write_user(user, &copied, sizeof(copied)) && original == replacement;

        passed &= safe_read_user(user, &copied, sizeof(copied)) && copied == replacement;
    }

    expect(passed, "user copy permissions and alias write-through");
}

} // namespace request_selftest
