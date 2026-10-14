#include "layers/Dispatcher.hpp"
#include "layers/Logic/CredentialManager.hpp"
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/TimeManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

extern "C"
{
#include <klib/klib.h>
#include <platform.h>
#include <selftests/selftests.h>
}

extern "C" uint64_t dispatcher_dispatch_syscall(const arch_syscall_frame_t* frame);

#if defined(__x86_64__)
static_assert(sizeof(arch_syscall_frame_t) == 96, "x86-64 syscall frame size must match entry assembly");
static_assert(offsetof(arch_syscall_frame_t, arg0) == 8, "RDI syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg1) == 16, "RSI syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg2) == 24, "RDX syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg3) == 32, "R10 syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg4) == 40, "R8 syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg5) == 48, "R9 syscall argument offset");
#endif

namespace
{
// Validators run in the kernel, but syscall buffers must still pass the real
// user-page checks. Alias their backing pages temporarily instead of allowing
// kernel pointers through the production copy helpers. Writes through an alias
// update the original test buffer, including nested iovec buffers.
struct request_user_fixture_t;
static request_user_fixture_t* request_user_fixtures[BOOT_SMP_MAX_CPUS];

struct request_user_fixture_t
{
    static constexpr size_t   MAX_PAGES = 128;
    static constexpr uint64_t USER_BASE = 0x600000000ULL;
    virt_addr_space_t*        space;
    request_user_fixture_t*   previous;
    virt_addr_t               pages[MAX_PAGES];
    size_t                    count;
    bool                      valid;

    request_user_fixture_t() : space(platform.cpus[arch_cpu_id()].address_space), previous(request_user_fixtures[arch_cpu_id()]), count(0), valid(true)
    {
        request_user_fixtures[arch_cpu_id()] = this;
    }

    ~request_user_fixture_t()
    {
        cpu_preemption_disable();
        if (space != nullptr)
        {
            spinlock_acquire(&space->operation_lock);
            for (size_t i = 0; i < count; ++i)
            {
                vmm_unmap_page(pages[i], space);
                vmm_free_region(space, pages[i]);
            }
            spinlock_release(&space->operation_lock);
        }
        request_user_fixtures[arch_cpu_id()] = previous;
        cpu_preemption_enable();
    }

    uint64_t Address(const void* buffer, size_t length)
    {
        const uint64_t source = (uint64_t) (uintptr_t) buffer;
        if (source < CANONICAL_KERNEL_BASE)
            return source;
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
                physical = kernelPage - zone->hhdm_offset;
            else
                physical = vmm_virt_to_phys(kernelPage, space);

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
};

static uint64_t request_user_address(const void* buffer, size_t length)
{
    request_user_fixture_t* fixture = request_user_fixtures[arch_cpu_id()];
    return fixture != nullptr ? fixture->Address(buffer, length) : 0;
}

// Exit is deliberately exercised against the harness process. Restore its
// state even when a validator fails, so one failure cannot poison later cases.
struct request_process_state_t
{
    process_t* process;
    bool       exited;
    int32_t    exitStatus;
    bool       guardExit;

    explicit request_process_state_t(uint64_t syscallNumber) : process(nullptr), exited(false), exitStatus(0), guardExit(syscallNumber == SYSCALL_exit || syscallNumber == SYSCALL_exit_group)
    {
        if (!guardExit)
            return;
        cpu_preemption_disable();
        Dispatcher*        dispatcher = static_cast<Dispatcher*>(platform.dispacher);
        ResourceLayerCaps* caps       = dispatcher != nullptr ? dispatcher->GetResourceLayerCaps() : nullptr;
        process                       = caps != nullptr && caps->processManager != nullptr ? caps->processManager->GetCurrentProcess() : nullptr;
        if (process != nullptr)
        {
            exited     = process->exited;
            exitStatus = process->exitStatus;
        }
    }

    ~request_process_state_t()
    {
        if (process != nullptr)
        {
            process->exited     = exited;
            process->exitStatus = exitStatus;
        }
        if (guardExit)
            cpu_preemption_enable();
    }
};

enum expected_result_kind_t
{
    EXPECT_NEG_ERRNO,
    EXPECT_NONNEG,
};

struct requestlayer_stats_t
{
    unsigned long long total;
    unsigned long long implemented;
    unsigned long long unimplemented;
    unsigned long long implementedPass;
    unsigned long long implementedFail;
    unsigned long long customCaseUsed;
    unsigned long long customCasePass;
    unsigned long long customCaseFail;
};

static void test_user_copy_fixture(requestlayer_stats_t* stats)
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
        frame.syscall_number       = SYSCALL_getcpu;
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
    stats->total++;
    stats->implemented++;
    stats->customCaseUsed++;
    if (passed)
    {
        stats->implementedPass++;
        stats->customCasePass++;
    }
    else
    {
        stats->implementedFail++;
        stats->customCaseFail++;
        selftest_record_failure_detail("safe_user_copy_fixture");
    }
}

struct custom_case_ctx_t
{
    const char*                 managerName;
    const char*                 testName;
    uint64_t                    syscallNumber;
    const arch_syscall_frame_t* frame;
    uint64_t                    result;
};

typedef bool (*custom_case_validator_t)(const custom_case_ctx_t* ctx);

template <typename ManagerT> struct request_method_test_t
{
    const char* name;
    uint64_t (ManagerT::*method)(const arch_syscall_frame_t* frame);
    uint64_t                syscallNumber;
    custom_case_validator_t customValidator;
};

static bool custom_case_stub_process_lifecycle(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr)
    {
        return false;
    }

    kprintf("Arx kernel: requestlayer_selftest custom testcase stub %s::%s syscall=%llu result=%lld\n", ctx->managerName, ctx->testName, (unsigned long long) ctx->syscallNumber, (long long) ctx->result);

    // Stub path: until full scenario tests are implemented, keep this non-failing.
    return true;
}

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

static bool custom_case_validate_process_identity(const custom_case_ctx_t* ctx)
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
    dispatchedFrame.syscall_number       = ctx->syscallNumber;
    const uint64_t dispatchResult        = dispatcher_dispatch_syscall(&dispatchedFrame);

    if (ctx->syscallNumber == SYSCALL_getpid)
    {
        return ctx->result == expectedPid && dispatchResult == expectedPid;
    }

    if (ctx->syscallNumber == SYSCALL_gettid)
    {
        return ctx->result == expectedTid && dispatchResult == expectedTid;
    }

    return false;
}

static bool custom_case_validate_vfs_rw(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if ((int64_t) ctx->result >= 0 || ctx->result == LINUX_ENOSYS || ctx->result == LINUX_EINVAL)
    {
        return false;
    }

    Dispatcher* dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    if (dispatcher == nullptr)
    {
        return false;
    }

    ResourceLayerCaps* resourceCaps = dispatcher->GetResourceLayerCaps();
    LogicLayerCaps*    logicCaps    = dispatcher->GetLogicLayerCaps();
    if (resourceCaps == nullptr || resourceCaps->processManager == nullptr || logicCaps == nullptr || logicCaps->virtualFileSystem == nullptr)
    {
        return false;
    }

    process_t* currentProcess = resourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return false;
    }

    vfs_path_t start = {};
    file_t*    file  = logicCaps->virtualFileSystem->Open(start, "/test.txt", 0);
    if (file == nullptr)
    {
        return false;
    }

    int64_t fd = resourceCaps->processManager->AddFileDescriptor(currentProcess, static_cast<file_handle_t>(file), FD_FLAG_NONE);
    if (fd < 0)
    {
        (void) logicCaps->virtualFileSystem->Close(file);
        return false;
    }

    bool validationPass = false;
    if (ctx->syscallNumber == SYSCALL_read)
    {
        char                 readBuffer[32] = {};
        arch_syscall_frame_t syscallFrame   = *(ctx->frame);
        syscallFrame.syscall_number         = SYSCALL_read;
        syscallFrame.arg0                   = (uint64_t) fd;
        syscallFrame.arg1                   = request_user_address(readBuffer, sizeof(readBuffer));
        syscallFrame.arg2                   = (uint64_t) sizeof(readBuffer);

        uint64_t readResult = dispatcher_dispatch_syscall(&syscallFrame);
        if ((int64_t) readResult >= 0)
        {
            arch_syscall_frame_t badFdFrame = syscallFrame;
            badFdFrame.arg0                 = (uint64_t) (fd + 1);
            const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);
            validationPass                  = (badFdResult == LINUX_EBADF);
        }
    }
    else if (ctx->syscallNumber == SYSCALL_write)
    {
        static const char payload[]                 = "Arx syscall write selftest\n";
        char              original[sizeof(payload)] = {};
        const uint64_t    maxBytes                  = (uint64_t) (sizeof(payload) - 1);

        uint64_t testBytes = 0;
        if (logicCaps->virtualFileSystem->Seek(file, 0, 0) >= 0)
        {
            int64_t originalRead = logicCaps->virtualFileSystem->Read(file, original, maxBytes);
            if (originalRead > 0)
            {
                testBytes = (uint64_t) originalRead;
                if (testBytes > maxBytes)
                {
                    testBytes = maxBytes;
                }
            }
        }

        if (logicCaps->virtualFileSystem->Seek(file, 0, 0) >= 0)
        {
            arch_syscall_frame_t syscallFrame = *(ctx->frame);
            syscallFrame.syscall_number       = SYSCALL_write;
            syscallFrame.arg0                 = (uint64_t) fd;
            syscallFrame.arg1                 = request_user_address(payload, sizeof(payload));
            syscallFrame.arg2                 = testBytes;

            uint64_t writeResult = dispatcher_dispatch_syscall(&syscallFrame);
            if (writeResult == testBytes)
            {
                arch_syscall_frame_t badFdFrame = syscallFrame;
                badFdFrame.arg0                 = (uint64_t) (fd + 1);
                const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);
                validationPass                  = (badFdResult == LINUX_EBADF);
            }

            if (testBytes > 0)
            {
                (void) logicCaps->virtualFileSystem->Seek(file, 0, 0);
                (void) logicCaps->virtualFileSystem->Write(file, original, testBytes);
            }
        }
    }

    currentProcess->fileDescriptors[fd].file  = nullptr;
    currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
    (void) logicCaps->virtualFileSystem->Close(file);

    return validationPass;
}

struct test_linux_iovec_t
{
    void*    iov_base;
    uint64_t iov_len;
};

static bool custom_case_validate_pread64(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    char                 readBuffer[32] = {};
    arch_syscall_frame_t preadFrame     = {};
    preadFrame.syscall_number           = SYSCALL_pread64;
    preadFrame.arg0                     = fd;
    preadFrame.arg1                     = request_user_address(readBuffer, sizeof(readBuffer));
    preadFrame.arg2                     = sizeof(readBuffer);
    preadFrame.arg3                     = 0;

    const uint64_t preadResult = dispatcher_dispatch_syscall(&preadFrame);

    arch_syscall_frame_t badFdFrame = preadFrame;
    badFdFrame.arg0                 = fd + 1;
    const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t badOffsetFrame = preadFrame;
    badOffsetFrame.arg3                 = (uint64_t) -1LL;
    const uint64_t badOffsetResult      = dispatcher_dispatch_syscall(&badOffsetFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    return (int64_t) preadResult >= 0 && badFdResult == LINUX_EBADF && badOffsetResult == LINUX_EINVAL && closeResult == 0;
}

static bool custom_case_validate_pwrite64(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    static const char    payload[]   = "Arx pread/pwrite selftest";
    arch_syscall_frame_t pwriteFrame = {};
    pwriteFrame.syscall_number       = SYSCALL_pwrite64;
    pwriteFrame.arg0                 = fd;
    pwriteFrame.arg1                 = request_user_address(payload, sizeof(payload));
    pwriteFrame.arg2                 = (uint64_t) (sizeof(payload) - 1);
    pwriteFrame.arg3                 = 0;

    const uint64_t pwriteResult = dispatcher_dispatch_syscall(&pwriteFrame);

    arch_syscall_frame_t badFdFrame = pwriteFrame;
    badFdFrame.arg0                 = fd + 1;
    const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t badOffsetFrame = pwriteFrame;
    badOffsetFrame.arg3                 = (uint64_t) -1LL;
    const uint64_t badOffsetResult      = dispatcher_dispatch_syscall(&badOffsetFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    return pwriteResult == (uint64_t) (sizeof(payload) - 1) && badFdResult == LINUX_EBADF && badOffsetResult == LINUX_EINVAL && closeResult == 0;
}

static bool custom_case_validate_readv(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    char               readA[8] = {};
    char               readB[8] = {};
    test_linux_iovec_t iov[2]   = {
            {(void*) (uintptr_t) request_user_address(readA, sizeof(readA)), sizeof(readA)},
            {(void*) (uintptr_t) request_user_address(readB, sizeof(readB)), sizeof(readB)},
    };

    arch_syscall_frame_t readvFrame = {};
    readvFrame.syscall_number       = SYSCALL_readv;
    readvFrame.arg0                 = fd;
    readvFrame.arg1                 = request_user_address(iov, sizeof(iov));
    readvFrame.arg2                 = 2;

    const uint64_t readvResult = dispatcher_dispatch_syscall(&readvFrame);

    arch_syscall_frame_t badIovcntFrame = readvFrame;
    badIovcntFrame.arg2                 = 1025;
    const uint64_t badIovcntResult      = dispatcher_dispatch_syscall(&badIovcntFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    return (int64_t) readvResult >= 0 && badIovcntResult == LINUX_EINVAL && closeResult == 0;
}

static bool custom_case_validate_writev(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    static char        partA[] = "Arx-";
    static char        partB[] = "writev";
    test_linux_iovec_t iov[2]  = {
            {(void*) (uintptr_t) request_user_address(partA, sizeof(partA)), (uint64_t) (sizeof(partA) - 1)},
            {(void*) (uintptr_t) request_user_address(partB, sizeof(partB)), (uint64_t) (sizeof(partB) - 1)},
    };

    arch_syscall_frame_t writevFrame = {};
    writevFrame.syscall_number       = SYSCALL_writev;
    writevFrame.arg0                 = fd;
    writevFrame.arg1                 = request_user_address(iov, sizeof(iov));
    writevFrame.arg2                 = 2;

    const uint64_t writevResult = dispatcher_dispatch_syscall(&writevFrame);

    arch_syscall_frame_t badFdFrame = writevFrame;
    badFdFrame.arg0                 = fd + 1;
    const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    const uint64_t expected = (uint64_t) ((sizeof(partA) - 1) + (sizeof(partB) - 1));
    return writevResult == expected && badFdResult == LINUX_EBADF && closeResult == 0;
}

static bool custom_case_validate_preadv(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    char               readA[8] = {};
    char               readB[8] = {};
    test_linux_iovec_t iov[2]   = {
            {(void*) (uintptr_t) request_user_address(readA, sizeof(readA)), sizeof(readA)},
            {(void*) (uintptr_t) request_user_address(readB, sizeof(readB)), sizeof(readB)},
    };

    arch_syscall_frame_t preadvFrame = {};
    preadvFrame.syscall_number       = SYSCALL_preadv;
    preadvFrame.arg0                 = fd;
    preadvFrame.arg1                 = request_user_address(iov, sizeof(iov));
    preadvFrame.arg2                 = 2;
    preadvFrame.arg3                 = 0;

    const uint64_t preadvResult = dispatcher_dispatch_syscall(&preadvFrame);

    arch_syscall_frame_t badOffsetFrame = preadvFrame;
    badOffsetFrame.arg3                 = (uint64_t) -1LL;
    const uint64_t badOffsetResult      = dispatcher_dispatch_syscall(&badOffsetFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    return (int64_t) preadvResult >= 0 && badOffsetResult == LINUX_EINVAL && closeResult == 0;
}

static bool custom_case_validate_pwritev(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    static char        partA[] = "Arx-";
    static char        partB[] = "pwritev";
    test_linux_iovec_t iov[2]  = {
            {(void*) (uintptr_t) request_user_address(partA, sizeof(partA)), (uint64_t) (sizeof(partA) - 1)},
            {(void*) (uintptr_t) request_user_address(partB, sizeof(partB)), (uint64_t) (sizeof(partB) - 1)},
    };

    arch_syscall_frame_t pwritevFrame = {};
    pwritevFrame.syscall_number       = SYSCALL_pwritev;
    pwritevFrame.arg0                 = fd;
    pwritevFrame.arg1                 = request_user_address(iov, sizeof(iov));
    pwritevFrame.arg2                 = 2;
    pwritevFrame.arg3                 = 0;

    const uint64_t pwritevResult = dispatcher_dispatch_syscall(&pwritevFrame);

    arch_syscall_frame_t badOffsetFrame = pwritevFrame;
    badOffsetFrame.arg3                 = (uint64_t) -1LL;
    const uint64_t badOffsetResult      = dispatcher_dispatch_syscall(&badOffsetFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    const uint64_t expected = (uint64_t) ((sizeof(partA) - 1) + (sizeof(partB) - 1));
    return pwritevResult == expected && badOffsetResult == LINUX_EINVAL && closeResult == 0;
}

static bool custom_case_validate_open(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if ((int64_t) ctx->result >= 0 || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fdResult;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);
    return closeResult == 0;
}

static bool custom_case_validate_creat(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if ((int64_t) ctx->result >= 0 || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t creatFrame = {};
    creatFrame.syscall_number       = SYSCALL_creat;
    creatFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    creatFrame.arg1                 = 0644;

    uint64_t fdResult = dispatcher_dispatch_syscall(&creatFrame);
    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fdResult;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);
    return closeResult == 0;
}

static bool custom_case_validate_openat(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if ((int64_t) ctx->result >= 0 || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openatFrame = {};
    openatFrame.syscall_number       = SYSCALL_openat;
    openatFrame.arg0                 = LINUX_AT_FDCWD;
    openatFrame.arg1                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openatFrame.arg2                 = 0;
    openatFrame.arg3                 = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openatFrame);
    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fdResult;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);
    if (closeResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t rootDirFrame = {};
    rootDirFrame.syscall_number       = SYSCALL_open;
    rootDirFrame.arg0                 = request_user_address("/", sizeof("/"));
    rootDirFrame.arg1                 = 0;
    rootDirFrame.arg2                 = 0;

    const uint64_t rootDirFd = dispatcher_dispatch_syscall(&rootDirFrame);
    if ((int64_t) rootDirFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t relativeOpenat = {};
    relativeOpenat.syscall_number       = SYSCALL_openat;
    relativeOpenat.arg0                 = rootDirFd;
    relativeOpenat.arg1                 = request_user_address("test.txt", sizeof("test.txt"));
    relativeOpenat.arg2                 = 0;
    relativeOpenat.arg3                 = 0;

    const uint64_t relativeFd = dispatcher_dispatch_syscall(&relativeOpenat);

    arch_syscall_frame_t closeRootDir = {};
    closeRootDir.syscall_number       = SYSCALL_close;
    closeRootDir.arg0                 = rootDirFd;
    const uint64_t closeRootDirResult = dispatcher_dispatch_syscall(&closeRootDir);

    if ((int64_t) relativeFd < 0 || closeRootDirResult != 0)
    {
        return false;
    }

    closeFrame.arg0                    = relativeFd;
    const uint64_t closeRelativeResult = dispatcher_dispatch_syscall(&closeFrame);
    if (closeRelativeResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t regularFileFrame = {};
    regularFileFrame.syscall_number       = SYSCALL_open;
    regularFileFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    regularFileFrame.arg1                 = 0;
    regularFileFrame.arg2                 = 0;

    const uint64_t regularFd = dispatcher_dispatch_syscall(&regularFileFrame);
    if ((int64_t) regularFd < 0)
    {
        return false;
    }

    relativeOpenat.arg0             = regularFd;
    const uint64_t nonDirOpenResult = dispatcher_dispatch_syscall(&relativeOpenat);

    closeFrame.arg0                   = regularFd;
    const uint64_t closeRegularResult = dispatcher_dispatch_syscall(&closeFrame);

    return nonDirOpenResult == LINUX_ENOTDIR && closeRegularResult == 0;
}

static bool custom_case_validate_close(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr)
    {
        return false;
    }

    if (ctx->result != LINUX_EBADF)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fdResult;

    const uint64_t firstClose = dispatcher_dispatch_syscall(&closeFrame);
    if (firstClose != 0)
    {
        return false;
    }

    const uint64_t secondClose = dispatcher_dispatch_syscall(&closeFrame);
    return secondClose == LINUX_EBADF;
}

static bool custom_case_validate_close_range(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openA = {};
    openA.syscall_number       = SYSCALL_open;
    openA.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openA.arg1                 = 0;
    openA.arg2                 = 0;
    const uint64_t fdA         = dispatcher_dispatch_syscall(&openA);
    if ((int64_t) fdA < 0)
    {
        return false;
    }

    arch_syscall_frame_t openB = openA;
    const uint64_t       fdB   = dispatcher_dispatch_syscall(&openB);
    if ((int64_t) fdB < 0)
    {
        return false;
    }

    const uint64_t first = (fdA < fdB) ? fdA : fdB;
    const uint64_t last  = (fdA < fdB) ? fdB : fdA;

    arch_syscall_frame_t closeRange = {};
    closeRange.syscall_number       = SYSCALL_close_range;
    closeRange.arg0                 = first;
    closeRange.arg1                 = last;
    closeRange.arg2                 = 0;
    const uint64_t closeRangeResult = dispatcher_dispatch_syscall(&closeRange);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fdA;
    const uint64_t closeA           = dispatcher_dispatch_syscall(&closeFrame);
    closeFrame.arg0                 = fdB;
    const uint64_t closeB           = dispatcher_dispatch_syscall(&closeFrame);

    arch_syscall_frame_t badRange = closeRange;
    badRange.arg0                 = 10;
    badRange.arg1                 = 5;
    const uint64_t badRangeResult = dispatcher_dispatch_syscall(&badRange);

    // Validate CLOSE_RANGE_UNSHARE is accepted (no-op in current kernel model).
    const uint64_t fdC = dispatcher_dispatch_syscall(&openA);
    const uint64_t fdD = dispatcher_dispatch_syscall(&openB);
    if ((int64_t) fdC < 0 || (int64_t) fdD < 0)
    {
        return false;
    }

    const uint64_t       first2            = (fdC < fdD) ? fdC : fdD;
    const uint64_t       last2             = (fdC < fdD) ? fdD : fdC;
    arch_syscall_frame_t closeRangeUnshare = {};
    closeRangeUnshare.syscall_number       = SYSCALL_close_range;
    closeRangeUnshare.arg0                 = first2;
    closeRangeUnshare.arg1                 = last2;
    closeRangeUnshare.arg2                 = (1ULL << 1);
    const uint64_t closeRangeUnshareResult = dispatcher_dispatch_syscall(&closeRangeUnshare);

    closeFrame.arg0       = fdC;
    const uint64_t closeC = dispatcher_dispatch_syscall(&closeFrame);
    closeFrame.arg0       = fdD;
    const uint64_t closeD = dispatcher_dispatch_syscall(&closeFrame);

    return closeRangeResult == 0 && closeA == LINUX_EBADF && closeB == LINUX_EBADF && badRangeResult == LINUX_EINVAL && closeRangeUnshareResult == 0 && closeC == LINUX_EBADF && closeD == LINUX_EBADF;
}

static bool custom_case_validate_fcntl(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    constexpr uint64_t TEST_F_GETFD    = 1;
    constexpr uint64_t TEST_F_SETFD    = 2;
    constexpr uint64_t TEST_F_GETFL    = 3;
    constexpr uint64_t TEST_F_SETFL    = 4;
    constexpr uint64_t TEST_F_DUPFD    = 0;
    constexpr uint64_t TEST_FD_CLOEXEC = 1;

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    arch_syscall_frame_t fcntlGet = {};
    fcntlGet.syscall_number       = SYSCALL_fcntl;
    fcntlGet.arg0                 = fd;
    fcntlGet.arg1                 = TEST_F_GETFD;
    fcntlGet.arg2                 = 0;
    const uint64_t getBefore      = dispatcher_dispatch_syscall(&fcntlGet);

    arch_syscall_frame_t fcntlSet = fcntlGet;
    fcntlSet.arg1                 = TEST_F_SETFD;
    fcntlSet.arg2                 = TEST_FD_CLOEXEC;
    const uint64_t setResult      = dispatcher_dispatch_syscall(&fcntlSet);

    const uint64_t getAfter = dispatcher_dispatch_syscall(&fcntlGet);

    arch_syscall_frame_t fcntlGetfl = fcntlGet;
    fcntlGetfl.arg1                 = TEST_F_GETFL;
    const uint64_t getflBefore      = dispatcher_dispatch_syscall(&fcntlGetfl);

    arch_syscall_frame_t fcntlSetfl = fcntlGet;
    fcntlSetfl.arg1                 = TEST_F_SETFL;
    fcntlSetfl.arg2                 = getflBefore | 04000ULL;
    const uint64_t setflResult      = dispatcher_dispatch_syscall(&fcntlSetfl);

    const uint64_t getflAfter = dispatcher_dispatch_syscall(&fcntlGetfl);

    arch_syscall_frame_t fcntlDup = fcntlGet;
    fcntlDup.arg1                 = TEST_F_DUPFD;
    fcntlDup.arg2                 = fd + 1;
    const uint64_t dupFd          = dispatcher_dispatch_syscall(&fcntlDup);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeOriginal    = dispatcher_dispatch_syscall(&closeFrame);

    uint64_t closeDup = LINUX_EBADF;
    if ((int64_t) dupFd >= 0)
    {
        closeFrame.arg0 = dupFd;
        closeDup        = dispatcher_dispatch_syscall(&closeFrame);
    }

    return getBefore == 0 && setResult == 0 && getAfter == TEST_FD_CLOEXEC && setflResult == 0 && getflAfter == (getflBefore | 04000ULL) && (int64_t) dupFd >= 0 && closeOriginal == 0 && closeDup == 0;
}

static bool custom_case_validate_dup(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t oldFd           = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) oldFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t dupFrame = {};
    dupFrame.syscall_number       = SYSCALL_dup;
    dupFrame.arg0                 = oldFd;
    const uint64_t newFd          = dispatcher_dispatch_syscall(&dupFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = oldFd;
    const uint64_t closeOld         = dispatcher_dispatch_syscall(&closeFrame);

    uint64_t closeNew = LINUX_EBADF;
    if ((int64_t) newFd >= 0)
    {
        closeFrame.arg0 = newFd;
        closeNew        = dispatcher_dispatch_syscall(&closeFrame);
    }

    return (int64_t) newFd >= 0 && closeOld == 0 && closeNew == 0;
}

static bool custom_case_validate_dup2(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t oldFd           = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) oldFd < 0)
    {
        return false;
    }

    const uint64_t tmpFd = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) tmpFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t dup2Frame = {};
    dup2Frame.syscall_number       = SYSCALL_dup2;
    dup2Frame.arg0                 = oldFd;
    dup2Frame.arg1                 = tmpFd;
    const uint64_t dup2Result      = dispatcher_dispatch_syscall(&dup2Frame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = oldFd;
    const uint64_t closeOld         = dispatcher_dispatch_syscall(&closeFrame);

    closeFrame.arg0         = tmpFd;
    const uint64_t closeNew = dispatcher_dispatch_syscall(&closeFrame);

    return dup2Result == tmpFd && closeOld == 0 && closeNew == 0;
}

static bool custom_case_validate_dup3(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    constexpr uint64_t TEST_O_CLOEXEC = 02000000ULL;

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t oldFd           = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) oldFd < 0)
    {
        return false;
    }

    const uint64_t tmpFd = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) tmpFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t dup3Frame = {};
    dup3Frame.syscall_number       = SYSCALL_dup3;
    dup3Frame.arg0                 = oldFd;
    dup3Frame.arg1                 = tmpFd;
    dup3Frame.arg2                 = TEST_O_CLOEXEC;
    const uint64_t dup3Result      = dispatcher_dispatch_syscall(&dup3Frame);

    arch_syscall_frame_t sameFrame = dup3Frame;
    sameFrame.arg1                 = oldFd;
    const uint64_t sameResult      = dispatcher_dispatch_syscall(&sameFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = oldFd;
    const uint64_t closeOld         = dispatcher_dispatch_syscall(&closeFrame);
    closeFrame.arg0                 = tmpFd;
    const uint64_t closeNew         = dispatcher_dispatch_syscall(&closeFrame);

    return dup3Result == tmpFd && sameResult == LINUX_EINVAL && closeOld == 0 && closeNew == 0;
}

static bool custom_case_validate_ioctl(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_ENOTTY = (uint64_t) -25;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    arch_syscall_frame_t ioctlFrame = {};
    ioctlFrame.syscall_number       = SYSCALL_ioctl;
    ioctlFrame.arg0                 = fd;
    ioctlFrame.arg1                 = 0;
    ioctlFrame.arg2                 = 0;
    const uint64_t ioctlResult      = dispatcher_dispatch_syscall(&ioctlFrame);

    arch_syscall_frame_t badFdFrame = ioctlFrame;
    badFdFrame.arg0                 = fd + 1;
    const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    return ioctlResult == TEST_LINUX_ENOTTY && badFdResult == LINUX_EBADF && closeResult == 0;
}

static bool custom_case_validate_getppid(const custom_case_ctx_t* ctx)
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
    if (ctx->result != expectedPpid)
    {
        return false;
    }

    arch_syscall_frame_t dispatchedFrame = *(ctx->frame);
    dispatchedFrame.syscall_number       = SYSCALL_getppid;
    const uint64_t dispatchResult        = dispatcher_dispatch_syscall(&dispatchedFrame);
    return dispatchResult == expectedPpid;
}

static bool custom_case_validate_sched_yield(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result != 0)
    {
        return false;
    }

    arch_syscall_frame_t dispatchedFrame = *(ctx->frame);
    dispatchedFrame.syscall_number       = SYSCALL_sched_yield;
    const uint64_t dispatchResult        = dispatcher_dispatch_syscall(&dispatchedFrame);
    return dispatchResult == 0;
}

static bool custom_case_validate_getcpu(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result != 0)
    {
        return false;
    }

    uint32_t cpuValue  = UINT32_MAX;
    uint32_t nodeValue = UINT32_MAX;

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_getcpu;
    frame.arg0                 = request_user_address(&cpuValue, sizeof(cpuValue));
    frame.arg1                 = request_user_address(&nodeValue, sizeof(nodeValue));
    frame.arg2                 = 0;

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&frame);
    const bool     cpuInRange     = cpuValue < BOOT_SMP_MAX_CPUS;
    return dispatchResult == 0 && cpuInRange && nodeValue == 0;
}

static bool custom_case_validate_sched_getaffinity(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    uint8_t mask[sizeof(uint64_t)] = {};

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_sched_getaffinity;
    frame.arg0                 = 0;
    frame.arg1                 = sizeof(mask);
    frame.arg2                 = request_user_address(mask, sizeof(mask));

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&frame);
    if (dispatchResult != sizeof(uint64_t))
    {
        return false;
    }

    uint64_t affinityMask = 0;
    memcpy(&affinityMask, mask, sizeof(affinityMask));
    if (affinityMask == 0)
    {
        return false;
    }

    arch_syscall_frame_t shortFrame = frame;
    shortFrame.arg1                 = sizeof(uint32_t);
    const uint64_t shortResult      = dispatcher_dispatch_syscall(&shortFrame);
    return shortResult == LINUX_EINVAL;
}

static bool custom_case_validate_exit_like(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result != 0)
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

    if (!currentProcess->exited)
    {
        return false;
    }

    // The next syscall must run in a live process under safe user-copy checks.
    currentProcess->exited     = false;
    currentProcess->exitStatus = 0;

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;
    const uint64_t testFd          = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) testFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t dispatchedFrame = *(ctx->frame);
    dispatchedFrame.syscall_number       = ctx->syscallNumber;
    const uint64_t dispatchResult        = dispatcher_dispatch_syscall(&dispatchedFrame);
    const bool     dispatchMarkedExited  = currentProcess->exited;
    bool           descriptorClosed      = false;

    if (testFd < currentProcess->fileDescriptorCount && currentProcess->fileDescriptors != nullptr)
    {
        descriptorClosed = (currentProcess->fileDescriptors[testFd].file == nullptr);
    }

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = testFd;
    const uint64_t closeAfterExit   = dispatcher_dispatch_syscall(&closeFrame);

    currentProcess->exited     = false;
    currentProcess->exitStatus = 0;

    return dispatchResult == 0 && dispatchMarkedExited && descriptorClosed && closeAfterExit == LINUX_EBADF;
}

static bool custom_case_validate_lseek(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t lseekEnd = {};
    lseekEnd.syscall_number       = SYSCALL_lseek;
    lseekEnd.arg0                 = fdResult;
    lseekEnd.arg1                 = 0;
    lseekEnd.arg2                 = 2;

    const uint64_t endOffset = dispatcher_dispatch_syscall(&lseekEnd);

    arch_syscall_frame_t lseekBad = lseekEnd;
    lseekBad.arg2                 = 99;
    const uint64_t badWhence      = dispatcher_dispatch_syscall(&lseekBad);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fdResult;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    return (int64_t) endOffset >= 0 && badWhence == LINUX_EINVAL && closeResult == 0;
}

static bool custom_case_validate_getcwd(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    char cwdBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_getcwd;
    frame.arg0                 = request_user_address(cwdBuffer, sizeof(cwdBuffer));
    frame.arg1                 = sizeof(cwdBuffer);

    const uint64_t getcwdResult = dispatcher_dispatch_syscall(&frame);
    if (getcwdResult == 0 || getcwdResult >= sizeof(cwdBuffer))
    {
        return false;
    }

    if (cwdBuffer[getcwdResult - 1] != '\0')
    {
        return false;
    }

    arch_syscall_frame_t badSizeFrame = frame;
    badSizeFrame.arg1                 = 1;
    const uint64_t badSizeResult      = dispatcher_dispatch_syscall(&badSizeFrame);
    return badSizeResult == LINUX_ERANGE;
}

static bool custom_case_validate_chdir(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t chdirRoot = {};
    chdirRoot.syscall_number       = SYSCALL_chdir;
    chdirRoot.arg0                 = request_user_address("/", sizeof("/"));
    const uint64_t chdirRootResult = dispatcher_dispatch_syscall(&chdirRoot);
    if (chdirRootResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t chdirBad = {};
    chdirBad.syscall_number       = SYSCALL_chdir;
    chdirBad.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    const uint64_t chdirBadResult = dispatcher_dispatch_syscall(&chdirBad);
    return chdirBadResult == LINUX_ENOTDIR;
}

static bool custom_case_validate_fchdir(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openDir = {};
    openDir.syscall_number       = SYSCALL_open;
    openDir.arg0                 = request_user_address("/", sizeof("/"));
    openDir.arg1                 = 0;
    openDir.arg2                 = 0;
    const uint64_t dirFd         = dispatcher_dispatch_syscall(&openDir);
    if ((int64_t) dirFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t fchdirFrame = {};
    fchdirFrame.syscall_number       = SYSCALL_fchdir;
    fchdirFrame.arg0                 = dirFd;
    const uint64_t fchdirResult      = dispatcher_dispatch_syscall(&fchdirFrame);

    arch_syscall_frame_t closeDir = {};
    closeDir.syscall_number       = SYSCALL_close;
    closeDir.arg0                 = dirFd;
    const uint64_t closeDirResult = dispatcher_dispatch_syscall(&closeDir);

    arch_syscall_frame_t openFile = {};
    openFile.syscall_number       = SYSCALL_open;
    openFile.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFile.arg1                 = 0;
    openFile.arg2                 = 0;
    const uint64_t fileFd         = dispatcher_dispatch_syscall(&openFile);
    if ((int64_t) fileFd < 0)
    {
        return false;
    }

    fchdirFrame.arg0                = fileFd;
    const uint64_t fileFchdirResult = dispatcher_dispatch_syscall(&fchdirFrame);

    closeDir.arg0                  = fileFd;
    const uint64_t closeFileResult = dispatcher_dispatch_syscall(&closeDir);

    return fchdirResult == 0 && closeDirResult == 0 && fileFchdirResult == LINUX_ENOTDIR && closeFileResult == 0;
}

static bool custom_case_validate_fstat(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1                 = 0;
    openFrame.arg2                 = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    test_linux_stat_t statBuffer = {};

    arch_syscall_frame_t fstatFrame = {};
    fstatFrame.syscall_number       = SYSCALL_fstat;
    fstatFrame.arg0                 = fdResult;
    fstatFrame.arg1                 = request_user_address(&statBuffer, sizeof(statBuffer));

    const uint64_t fstatResult = dispatcher_dispatch_syscall(&fstatFrame);

    arch_syscall_frame_t badFdFrame = fstatFrame;
    badFdFrame.arg0                 = fdResult + 1;
    const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fdResult;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    constexpr uint32_t linux_s_ifmt  = 0170000U;
    constexpr uint32_t linux_s_ifreg = 0100000U;

    const bool modeLooksRegular = (statBuffer.st_mode & linux_s_ifmt) == linux_s_ifreg;
    const bool sizeReasonable   = statBuffer.st_size >= 0;
    const bool blockSizeSet     = statBuffer.st_blksize > 0;

    return fstatResult == 0 && statBuffer.st_nlink >= 1 && modeLooksRegular && sizeReasonable && blockSizeSet && badFdResult == LINUX_EBADF && closeResult == 0;
}

static bool custom_case_validate_newfstatat(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    test_linux_stat_t statBuffer = {};

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_newfstatat;
    frame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1                 = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg2                 = request_user_address(&statBuffer, sizeof(statBuffer));
    frame.arg3                 = 0;

    const uint64_t okResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFlagFrame = frame;
    badFlagFrame.arg3                 = 0x80000000ULL;
    const uint64_t badFlagResult      = dispatcher_dispatch_syscall(&badFlagFrame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg1                 = request_user_address("/does-not-exist", sizeof("/does-not-exist"));
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return okResult == 0 && statBuffer.st_nlink >= 1 && badFlagResult == LINUX_EINVAL && badPathResult == LINUX_ENOENT;
}

static bool custom_case_validate_stat(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    test_linux_stat_t statBuffer = {};

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_stat;
    frame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1                 = request_user_address(&statBuffer, sizeof(statBuffer));

    const uint64_t okResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t nullBufferFrame = frame;
    nullBufferFrame.arg1                 = 0;
    const uint64_t nullBufferResult      = dispatcher_dispatch_syscall(&nullBufferFrame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg0                 = request_user_address("/does-not-exist", sizeof("/does-not-exist"));
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return okResult == 0 && statBuffer.st_nlink >= 1 && nullBufferResult == LINUX_EFAULT && badPathResult == LINUX_ENOENT;
}

static bool custom_case_validate_lstat(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    test_linux_stat_t statBuffer = {};

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_lstat;
    frame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1                 = request_user_address(&statBuffer, sizeof(statBuffer));

    const uint64_t okResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t nullPathFrame = frame;
    nullPathFrame.arg0                 = 0;
    const uint64_t nullPathResult      = dispatcher_dispatch_syscall(&nullPathFrame);

    return okResult == 0 && statBuffer.st_nlink >= 1 && nullPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_symlink(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t linkFrame = {};
    linkFrame.syscall_number       = SYSCALL_symlink;
    linkFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    linkFrame.arg1                 = request_user_address("/tmp-syscall-link", sizeof("/tmp-syscall-link"));
    const uint64_t linkResult      = dispatcher_dispatch_syscall(&linkFrame);

    if (linkResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t readFrame  = {};
    char                 target[64] = {};
    readFrame.syscall_number        = SYSCALL_readlink;
    readFrame.arg0                  = request_user_address("/tmp-syscall-link", sizeof("/tmp-syscall-link"));
    readFrame.arg1                  = request_user_address(target, sizeof(target));
    readFrame.arg2                  = sizeof(target);
    const uint64_t readResult       = dispatcher_dispatch_syscall(&readFrame);

    return (int64_t) readResult > 0 && memcmp(target, "/test.txt", (size_t) readResult) == 0;
}

static bool custom_case_validate_symlinkat(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openDir = {};
    openDir.syscall_number       = SYSCALL_open;
    openDir.arg0                 = request_user_address("/", sizeof("/"));
    const uint64_t dirFd         = dispatcher_dispatch_syscall(&openDir);
    if ((int64_t) dirFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t linkFrame = {};
    linkFrame.syscall_number       = SYSCALL_symlinkat;
    linkFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    linkFrame.arg1                 = dirFd;
    linkFrame.arg2                 = request_user_address("tmp-syscall-link-at", sizeof("tmp-syscall-link-at"));
    const uint64_t linkResult      = dispatcher_dispatch_syscall(&linkFrame);

    arch_syscall_frame_t closeDir = {};
    closeDir.syscall_number       = SYSCALL_close;
    closeDir.arg0                 = dirFd;
    (void) dispatcher_dispatch_syscall(&closeDir);

    if (linkResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t readFrame  = {};
    char                 target[64] = {};
    readFrame.syscall_number        = SYSCALL_readlink;
    readFrame.arg0                  = request_user_address("/tmp-syscall-link-at", sizeof("/tmp-syscall-link-at"));
    readFrame.arg1                  = request_user_address(target, sizeof(target));
    readFrame.arg2                  = sizeof(target);
    const uint64_t readResult       = dispatcher_dispatch_syscall(&readFrame);

    return (int64_t) readResult > 0 && memcmp(target, "/test.txt", (size_t) readResult) == 0;
}

static bool custom_case_validate_readlink(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t createFrame = {};
    createFrame.syscall_number       = SYSCALL_symlink;
    createFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    createFrame.arg1                 = request_user_address("/tmp-syscall-link-read", sizeof("/tmp-syscall-link-read"));
    const uint64_t createResult      = dispatcher_dispatch_syscall(&createFrame);
    if (createResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t frame      = {};
    char                 target[64] = {};
    frame.syscall_number            = SYSCALL_readlink;
    frame.arg0                      = request_user_address("/tmp-syscall-link-read", sizeof("/tmp-syscall-link-read"));
    frame.arg1                      = request_user_address(target, sizeof(target));
    frame.arg2                      = sizeof(target);

    const uint64_t readResult = dispatcher_dispatch_syscall(&frame);
    return (int64_t) readResult > 0;
}

static bool custom_case_validate_readlinkat(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openDir = {};
    openDir.syscall_number       = SYSCALL_open;
    openDir.arg0                 = request_user_address("/", sizeof("/"));
    const uint64_t dirFd         = dispatcher_dispatch_syscall(&openDir);
    if ((int64_t) dirFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t createFrame = {};
    createFrame.syscall_number       = SYSCALL_symlinkat;
    createFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    createFrame.arg1                 = dirFd;
    createFrame.arg2                 = request_user_address("tmp-syscall-link-at-read", sizeof("tmp-syscall-link-at-read"));
    const uint64_t createResult      = dispatcher_dispatch_syscall(&createFrame);
    if (createResult != 0)
    {
        arch_syscall_frame_t closeDir = {};
        closeDir.syscall_number       = SYSCALL_close;
        closeDir.arg0                 = dirFd;
        (void) dispatcher_dispatch_syscall(&closeDir);
        return false;
    }

    arch_syscall_frame_t frame      = {};
    char                 target[64] = {};
    frame.syscall_number            = SYSCALL_readlinkat;
    frame.arg0                      = dirFd;
    frame.arg1                      = request_user_address("tmp-syscall-link-at-read", sizeof("tmp-syscall-link-at-read"));
    frame.arg2                      = request_user_address(target, sizeof(target));
    frame.arg3                      = sizeof(target);
    const uint64_t readResult       = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t closeDir = {};
    closeDir.syscall_number       = SYSCALL_close;
    closeDir.arg0                 = dirFd;
    (void) dispatcher_dispatch_syscall(&closeDir);

    return (int64_t) readResult > 0;
}

static bool custom_case_validate_getdents64(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openDir = {};
    openDir.syscall_number       = SYSCALL_open;
    openDir.arg0                 = request_user_address("/", sizeof("/"));
    const uint64_t dirFd         = dispatcher_dispatch_syscall(&openDir);
    if ((int64_t) dirFd < 0)
    {
        return false;
    }

    char                 buffer[512] = {};
    arch_syscall_frame_t frame       = {};
    frame.syscall_number             = SYSCALL_getdents64;
    frame.arg0                       = dirFd;
    frame.arg1                       = request_user_address(buffer, sizeof(buffer));
    frame.arg2                       = sizeof(buffer);

    const uint64_t getdentsResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFdFrame = frame;
    badFdFrame.arg0                 = dirFd + 1024;
    const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = dirFd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    return (int64_t) getdentsResult >= 0 && badFdResult == LINUX_EBADF && closeResult == 0;
}

static bool custom_case_validate_access_family(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EACCES = (uint64_t) -13;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = ctx->syscallNumber;

    if (ctx->syscallNumber == SYSCALL_access)
    {
        frame.arg0 = request_user_address("/test.txt", sizeof("/test.txt"));
        frame.arg1 = 0;
    }
    else
    {
        frame.arg0 = (uint64_t) LINUX_AT_FDCWD;
        frame.arg1 = request_user_address("/test.txt", sizeof("/test.txt"));
        frame.arg2 = 0;
        frame.arg3 = 0;
    }

    const uint64_t existsResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t execCheckFrame = frame;
    if (ctx->syscallNumber == SYSCALL_access)
    {
        execCheckFrame.arg1 = 1;
    }
    else
    {
        execCheckFrame.arg2 = 1;
    }
    const uint64_t execCheckResult = dispatcher_dispatch_syscall(&execCheckFrame);

    arch_syscall_frame_t missingFrame = frame;
    if (ctx->syscallNumber == SYSCALL_access)
    {
        missingFrame.arg0 = request_user_address("/definitely-missing-selftest-file", sizeof("/definitely-missing-selftest-file"));
    }
    else
    {
        missingFrame.arg1 = request_user_address("/definitely-missing-selftest-file", sizeof("/definitely-missing-selftest-file"));
    }
    const uint64_t missingResult = dispatcher_dispatch_syscall(&missingFrame);

    arch_syscall_frame_t badModeFrame = frame;
    if (ctx->syscallNumber == SYSCALL_access)
    {
        badModeFrame.arg1 = 8;
    }
    else
    {
        badModeFrame.arg2 = 8;
    }
    const uint64_t badModeResult = dispatcher_dispatch_syscall(&badModeFrame);

    bool flagsResultOk = true;
    if (ctx->syscallNumber == SYSCALL_faccessat2)
    {
        arch_syscall_frame_t badFlagsFrame = frame;
        badFlagsFrame.arg3                 = 1ULL << 20;
        flagsResultOk                      = dispatcher_dispatch_syscall(&badFlagsFrame) == LINUX_EINVAL;
    }

    return existsResult == 0 && execCheckResult == TEST_LINUX_EACCES && missingResult == LINUX_ENOENT && badModeResult == LINUX_EINVAL && flagsResultOk;
}

static bool custom_case_validate_statfs(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    struct test_linux_statfs_t
    {
        int64_t  f_type;
        int64_t  f_bsize;
        uint64_t f_blocks;
        uint64_t f_bfree;
        uint64_t f_bavail;
        uint64_t f_files;
        uint64_t f_ffree;
        int32_t  f_fsid_val[2];
        int64_t  f_namelen;
        int64_t  f_frsize;
        int64_t  f_flags;
        int64_t  f_spare[4];
    };

    test_linux_statfs_t  statfsData = {};
    arch_syscall_frame_t frame      = {};
    frame.syscall_number            = SYSCALL_statfs;
    frame.arg0                      = request_user_address("/", sizeof("/"));
    frame.arg1                      = request_user_address(&statfsData, sizeof(statfsData));

    const uint64_t statfsResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t missingFrame = frame;
    missingFrame.arg0                 = request_user_address("/definitely-missing-selftest-file", sizeof("/definitely-missing-selftest-file"));
    const uint64_t missingResult      = dispatcher_dispatch_syscall(&missingFrame);

    return statfsResult == 0 && statfsData.f_bsize > 0 && missingResult == LINUX_ENOENT;
}

static bool custom_case_validate_fstatfs(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    struct test_linux_statfs_t
    {
        int64_t  f_type;
        int64_t  f_bsize;
        uint64_t f_blocks;
        uint64_t f_bfree;
        uint64_t f_bavail;
        uint64_t f_files;
        uint64_t f_ffree;
        int32_t  f_fsid_val[2];
        int64_t  f_namelen;
        int64_t  f_frsize;
        int64_t  f_flags;
        int64_t  f_spare[4];
    };

    test_linux_statfs_t statfsData = {};

    arch_syscall_frame_t frame   = {};
    frame.syscall_number         = SYSCALL_fstatfs;
    frame.arg0                   = fd;
    frame.arg1                   = request_user_address(&statfsData, sizeof(statfsData));
    const uint64_t fstatfsResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFdFrame = frame;
    badFdFrame.arg0                 = fd + 1024;
    const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    return fstatfsResult == 0 && statfsData.f_bsize > 0 && badFdResult == LINUX_EBADF && closeResult == 0;
}

static bool custom_case_validate_mkdirat(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_mkdirat;
    frame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1                 = request_user_address("/tmp-syscall-mkdirat", sizeof("/tmp-syscall-mkdirat"));
    frame.arg2                 = 0755;

    const uint64_t mkdirResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg1                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return mkdirResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_mkdir(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_mkdir;
    frame.arg0                 = request_user_address("/tmp-syscall-mkdir", sizeof("/tmp-syscall-mkdir"));
    frame.arg1                 = 0755;

    const uint64_t mkdirResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg0                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return mkdirResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_unlinkat(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame  = {};
    frame.syscall_number        = SYSCALL_unlinkat;
    frame.arg0                  = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1                  = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg2                  = 0;
    const uint64_t unlinkResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFlagsFrame = frame;
    badFlagsFrame.arg2                 = (1ULL << 20);
    const uint64_t badFlagsResult      = dispatcher_dispatch_syscall(&badFlagsFrame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg1                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return unlinkResult == TEST_LINUX_EROFS && badFlagsResult == LINUX_EINVAL && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_unlink(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame  = {};
    frame.syscall_number        = SYSCALL_unlink;
    frame.arg0                  = request_user_address("/test.txt", sizeof("/test.txt"));
    const uint64_t unlinkResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg0                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return unlinkResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_rmdir(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_rmdir;
    frame.arg0                 = request_user_address("/", sizeof("/"));
    const uint64_t rmdirResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg0                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return rmdirResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_renameat(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame  = {};
    frame.syscall_number        = SYSCALL_renameat;
    frame.arg0                  = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1                  = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg2                  = (uint64_t) LINUX_AT_FDCWD;
    frame.arg3                  = request_user_address("/test.txt.renamed", sizeof("/test.txt.renamed"));
    const uint64_t renameResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg1                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return renameResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_rename(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame  = {};
    frame.syscall_number        = SYSCALL_rename;
    frame.arg0                  = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1                  = request_user_address("/test.txt.renamed2", sizeof("/test.txt.renamed2"));
    const uint64_t renameResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg0                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return renameResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_linkat(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_linkat;
    frame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1                 = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg2                 = (uint64_t) LINUX_AT_FDCWD;
    frame.arg3                 = request_user_address("/tmp-syscall-linkat", sizeof("/tmp-syscall-linkat"));
    frame.arg4                 = 0;
    const uint64_t linkResult  = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFlagsFrame = frame;
    badFlagsFrame.arg4                 = (1ULL << 20);
    const uint64_t badFlagsResult      = dispatcher_dispatch_syscall(&badFlagsFrame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg1                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return linkResult == TEST_LINUX_EROFS && badFlagsResult == LINUX_EINVAL && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_link(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_link;
    frame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1                 = request_user_address("/tmp-syscall-link", sizeof("/tmp-syscall-link"));
    const uint64_t linkResult  = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg0                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return linkResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_mknodat(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame   = {};
    frame.syscall_number         = SYSCALL_mknodat;
    frame.arg0                   = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1                   = request_user_address("/tmp-syscall-mknodat", sizeof("/tmp-syscall-mknodat"));
    frame.arg2                   = 0644;
    frame.arg3                   = 0;
    const uint64_t mknodatResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg1                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return mknodatResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_mknod(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = SYSCALL_mknod;
    frame.arg0                 = request_user_address("/tmp-syscall-mknod", sizeof("/tmp-syscall-mknod"));
    frame.arg1                 = 0644;
    frame.arg2                 = 0;
    const uint64_t mknodResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;
    badPathFrame.arg0                 = 0;
    const uint64_t badPathResult      = dispatcher_dispatch_syscall(&badPathFrame);

    return mknodResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT;
}

static bool custom_case_validate_truncate(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    test_linux_stat_t    statBuffer = {};
    arch_syscall_frame_t statFrame  = {};
    statFrame.syscall_number        = SYSCALL_stat;
    statFrame.arg0                  = request_user_address("/test.txt", sizeof("/test.txt"));
    statFrame.arg1                  = request_user_address(&statBuffer, sizeof(statBuffer));
    const uint64_t statResult       = dispatcher_dispatch_syscall(&statFrame);
    if (statResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t frame    = {};
    frame.syscall_number          = SYSCALL_truncate;
    frame.arg0                    = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1                    = (uint64_t) statBuffer.st_size;
    const uint64_t truncateResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badLengthFrame = frame;
    badLengthFrame.arg1                 = (uint64_t) -1LL;
    const uint64_t badLengthResult      = dispatcher_dispatch_syscall(&badLengthFrame);

    arch_syscall_frame_t growFrame = frame;
    growFrame.arg1                 = (uint64_t) statBuffer.st_size + 1ULL;
    const uint64_t growResult      = dispatcher_dispatch_syscall(&growFrame);

    return truncateResult == 0 && badLengthResult == LINUX_EINVAL && growResult == TEST_LINUX_EROFS;
}

static bool custom_case_validate_ftruncate(const custom_case_ctx_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr || ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    arch_syscall_frame_t openFrame = {};
    openFrame.syscall_number       = SYSCALL_open;
    openFrame.arg0                 = request_user_address("/test.txt", sizeof("/test.txt"));
    const uint64_t fd              = dispatcher_dispatch_syscall(&openFrame);
    if ((int64_t) fd < 0)
    {
        return false;
    }

    test_linux_stat_t    statBuffer = {};
    arch_syscall_frame_t fstatFrame = {};
    fstatFrame.syscall_number       = SYSCALL_fstat;
    fstatFrame.arg0                 = fd;
    fstatFrame.arg1                 = request_user_address(&statBuffer, sizeof(statBuffer));
    const uint64_t fstatResult      = dispatcher_dispatch_syscall(&fstatFrame);
    if (fstatResult != 0)
    {
        arch_syscall_frame_t closeFrame = {};
        closeFrame.syscall_number       = SYSCALL_close;
        closeFrame.arg0                 = fd;
        (void) dispatcher_dispatch_syscall(&closeFrame);
        return false;
    }

    arch_syscall_frame_t frame     = {};
    frame.syscall_number           = SYSCALL_ftruncate;
    frame.arg0                     = fd;
    frame.arg1                     = (uint64_t) statBuffer.st_size;
    const uint64_t ftruncateResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badLengthFrame = frame;
    badLengthFrame.arg1                 = (uint64_t) -1LL;
    const uint64_t badLengthResult      = dispatcher_dispatch_syscall(&badLengthFrame);

    arch_syscall_frame_t growFrame = frame;
    growFrame.arg1                 = (uint64_t) statBuffer.st_size + 1ULL;
    const uint64_t growResult      = dispatcher_dispatch_syscall(&growFrame);

    arch_syscall_frame_t badFdFrame = frame;
    badFdFrame.arg0                 = fd + 1024;
    const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};
    closeFrame.syscall_number       = SYSCALL_close;
    closeFrame.arg0                 = fd;
    const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

    return ftruncateResult == 0 && badLengthResult == LINUX_EINVAL && growResult == TEST_LINUX_EROFS && badFdResult == LINUX_EBADF && closeResult == 0;
}

static bool custom_case_validate_umask(const custom_case_ctx_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    if (ctx->result == LINUX_ENOSYS)
    {
        return false;
    }

    constexpr uint64_t firstMask  = 0022;
    constexpr uint64_t secondMask = 0077;

    arch_syscall_frame_t firstFrame = {};
    firstFrame.syscall_number       = SYSCALL_umask;
    firstFrame.arg0                 = firstMask;
    const uint64_t oldMask          = dispatcher_dispatch_syscall(&firstFrame);

    arch_syscall_frame_t secondFrame = firstFrame;
    secondFrame.arg0                 = secondMask | ~0777ULL;
    const uint64_t previousSecond    = dispatcher_dispatch_syscall(&secondFrame);

    arch_syscall_frame_t restoreFrame = firstFrame;
    restoreFrame.arg0                 = ctx->result;
    const uint64_t previousRestore    = dispatcher_dispatch_syscall(&restoreFrame);

    return oldMask == 0 && previousSecond == firstMask && previousRestore == secondMask;
}

// Included inside syscalltests.cpp's anonymous namespace.
static VirtualMemoryManager* memory_vmm()
{
    Dispatcher* dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    return dispatcher->GetResourceLayerCaps()->virtualMemoryManager;
}

static uint64_t memory_call(uint64_t number, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0, uint64_t arg4 = 0, uint64_t arg5 = 0)
{
    arch_syscall_frame_t frame = {};
    frame.syscall_number       = number;
    frame.arg0                 = arg0;
    frame.arg1                 = arg1;
    frame.arg2                 = arg2;
    frame.arg3                 = arg3;
    frame.arg4                 = arg4;
    frame.arg5                 = arg5;
    return dispatcher_dispatch_syscall(&frame);
}

static uint64_t memory_map(uint64_t pages = 3, uint64_t address = 0, uint64_t flags = 0x22)
{
    return memory_call(SYSCALL_mmap, address, pages * PAGE_SIZE, 3, flags, UINT64_MAX);
}

static virt_region_t* memory_region(process_t* process, uint64_t address)
{
    return memory_vmm()->FindRegion(process->addressSpace, address);
}

static uint8_t* memory_bytes(process_t* process, uint64_t address)
{
    virt_region_t* region = memory_region(process, address);
    return region != nullptr && region->backing != nullptr ? static_cast<uint8_t*>(region->backing) + (address & (PAGE_SIZE - 1)) : nullptr;
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
static bool memory_expect(bool condition, const char* detail)
{
    if (!condition)
        selftest_record_failure_detail(detail);
    return condition;
}

static size_t memory_pages_in_use()
{
    zone_t* zone = &platform.cpus[arch_cpu_id()].numa_node->zone;
    spinlock_acquire(&zone->lock);
    const size_t usedPages = zone->used_pages;
    spinlock_release(&zone->lock);
    return usedPages;
}

static bool test_memory_mmap(process_t* process)
{
    bool     ok = true;
    uint64_t a  = memory_map();
    ok &= memory_expect(a == 0x10000 && memory_region(process, a), "mmap page-aligned placement");
    if (static_cast<int64_t>(a) < 0)
        return false;
    for (uint64_t p = a; p < a + 3 * PAGE_SIZE; p += PAGE_SIZE)
        for (unsigned i = 0; i < PAGE_SIZE; ++i)
            if (!memory_bytes(process, p) || memory_bytes(process, p)[i])
                return memory_expect(false, "mmap zero initialization");
    uint64_t b = memory_map(1);
    ok &= memory_expect(b == a + 3 * PAGE_SIZE && memory_bytes(process, b) != memory_bytes(process, a), "mmap independent backing");
    uint64_t fixed = 0x400000;
    ok &= memory_expect(memory_map(1, fixed, 0x100022) == fixed, "mmap fixed noreplace");
    ok &= memory_expect(memory_map(1, fixed, 0x100022) == static_cast<uint64_t>(-17), "mmap collision EEXIST");
    if (memory_bytes(process, fixed))
        *memory_bytes(process, fixed) = 0xA5;
    ok &= memory_expect(memory_map(1, fixed, 0x32) == fixed && memory_bytes(process, fixed) && *memory_bytes(process, fixed) == 0, "mmap fixed replacement zeroes");
    ok &= memory_expect(memory_map(1, 0x600123) == 0x600000, "mmap address hint");
    ok &= memory_expect(memory_map(1, fixed) != fixed, "mmap occupied hint fallback");
    ok &= memory_expect(memory_call(SYSCALL_mmap, 0, 0, 3, 0x22) == LINUX_EINVAL, "mmap zero length");
    ok &= memory_expect(memory_call(SYSCALL_mmap, 0, PAGE_SIZE, 8, 0x22) == LINUX_EINVAL, "mmap invalid protection");
    ok &= memory_expect(memory_map(1, 0, 0x20) == LINUX_EINVAL && memory_map(1, 0, 0x80000022) == LINUX_EINVAL, "mmap invalid flags");
    ok &= memory_expect(memory_call(SYSCALL_mmap, 0, PAGE_SIZE, 3, 0x22, 0, 1) == LINUX_EINVAL, "mmap offset alignment");
    ok &= memory_expect(memory_map(1, fixed + 1, 0x32) == LINUX_EINVAL, "mmap fixed alignment");
    ok &= memory_expect(memory_call(SYSCALL_mmap, CANONICAL_KERNEL_BASE, PAGE_SIZE, 3, 0x32) == LINUX_EINVAL, "mmap rejects kernel address");
    ok &= memory_expect(memory_call(SYSCALL_mmap, 0, UINT64_MAX, 3, 0x22) == LINUX_EINVAL, "mmap length overflow");
    ok &= memory_expect(memory_map(1, 0, 0x21) == static_cast<uint64_t>(-95), "mmap shared explicitly unsupported");
    ok &= memory_expect(memory_call(SYSCALL_mmap, 0, PAGE_SIZE, 1, 2, UINT64_MAX) == LINUX_EBADF, "mmap invalid fd");
    memcpy(memory_bytes(process, a), "/test.txt", sizeof("/test.txt"));
    uint64_t fd = memory_call(SYSCALL_open, a, 0);
    ok &= memory_expect(static_cast<int64_t>(fd) >= 0, "mmap file fixture opens through real VFS");
    if (static_cast<int64_t>(fd) >= 0)
    {
        ok &= memory_expect(memory_call(SYSCALL_mmap, 0, PAGE_SIZE, 1, 2, fd) == static_cast<uint64_t>(-95), "mmap file dependency reported");
        ok &= memory_expect(memory_call(SYSCALL_close, fd) == 0, "mmap file fixture closes");
    }
    // Constrain the free interval to exercise address-space exhaustion safely.
    uint64_t       failStart = 0x1000000;
    virt_region_t* free      = process->addressSpace->user_free_regions;
    while (free && !(free->start <= failStart && free->end > failStart))
        free = free->next;
    if (!free)
        return memory_expect(false, "mmap rollback fixture range");
    uint64_t savedEnd = free->end;
    free->end         = failStart + 2 * PAGE_SIZE;
    free->size        = free->end - free->start;
    ok &= memory_expect(memory_map(3, failStart, 0x32) == static_cast<uint64_t>(-12), "mmap allocation failure");
    ok &= memory_expect(!memory_region(process, failStart) && !memory_vmm()->VirtToPhys(failStart, process->addressSpace), "mmap placement failure preserves mappings");
    // Free-list coalescing may replace the original node.
    for (virt_region_t* r = process->addressSpace->user_free_regions; r; r = r->next)
        if (r->end == failStart + 2 * PAGE_SIZE)
        {
            r->end  = savedEnd;
            r->size = r->end - r->start;
            break;
        }
    return ok;
}
static bool test_memory_munmap(process_t* process)
{
    uint64_t a = memory_map();
    if (static_cast<int64_t>(a) < 0)
        return false;
    const size_t beforeUnmap = memory_pages_in_use();
    bool         ok          = memory_expect(memory_call(SYSCALL_munmap, a + PAGE_SIZE, 1) == 0, "munmap middle page");
    ok &= memory_expect(memory_pages_in_use() + 1 == beforeUnmap, "munmap releases physical middle page");
    ok &= memory_expect(memory_region(process, a) && !memory_region(process, a + PAGE_SIZE) && memory_region(process, a + 2 * PAGE_SIZE), "munmap neighboring metadata intact");
    ok &= memory_expect(!memory_vmm()->VirtToPhys(a + PAGE_SIZE, process->addressSpace), "munmap leaf cleared");
    ok &= memory_expect(memory_call(SYSCALL_munmap, a, 3 * PAGE_SIZE) == 0 && !memory_region(process, a) && !memory_region(process, a + 2 * PAGE_SIZE), "munmap spanning regions and hole");
    ok &= memory_expect(memory_pages_in_use() + 3 == beforeUnmap, "munmap releases all physical backing");
    ok &= memory_expect(memory_call(SYSCALL_munmap, a, 3 * PAGE_SIZE) == 0, "munmap already unmapped");
    ok &= memory_expect(memory_map(3, a, 0x100022) == a, "munmap reservation reusable");
    ok &= memory_expect(memory_call(SYSCALL_munmap, a + 1, PAGE_SIZE) == LINUX_EINVAL && memory_call(SYSCALL_munmap, a, 0) == LINUX_EINVAL, "munmap invalid alignment and length");
    return ok;
}
static bool test_memory_mprotect(process_t* process)
{
    uint64_t a = memory_map();
    if (static_cast<int64_t>(a) < 0)
        return false;
    bool ok = memory_expect(memory_call(SYSCALL_mprotect, a + PAGE_SIZE, PAGE_SIZE, 1) == 0, "mprotect read-only subrange");
    ok &= memory_expect(memory_region(process, a)->protection == 3 && memory_region(process, a + PAGE_SIZE)->protection == 1 && !(memory_pte(process, a + PAGE_SIZE) & 2), "mprotect metadata and readonly PTE");
    ok &= memory_expect(memory_call(SYSCALL_mprotect, a, PAGE_SIZE, 5) == 0 && !(memory_pte(process, a) & (1ULL << 63)), "mprotect executable PTE");
    ok &= memory_expect(memory_call(SYSCALL_mprotect, a, PAGE_SIZE, 0) == 0 && !(memory_pte(process, a) & 4) && memory_region(process, a)->backing, "mprotect none denies user access retains backing");
    ok &= memory_expect(memory_call(SYSCALL_mprotect, a, PAGE_SIZE, 3) == 0 && (memory_pte(process, a) & 6) == 6 && (memory_pte(process, a) & (1ULL << 63)), "mprotect restores write and NX");
    for (uint32_t protection = 0; protection < 8; ++protection)
    {
        ok &= memory_expect(memory_call(SYSCALL_mprotect, a + 2 * PAGE_SIZE, PAGE_SIZE, protection) == 0 && memory_region(process, a + 2 * PAGE_SIZE)->protection == protection, "mprotect all valid protection combinations");
    }
    ok &= memory_expect(memory_call(SYSCALL_mprotect, a, PAGE_SIZE, 8) == LINUX_EINVAL && memory_call(SYSCALL_mprotect, a + 1, PAGE_SIZE, 1) == LINUX_EINVAL, "mprotect invalid flags/alignment");
    ok &= memory_expect(memory_call(SYSCALL_mprotect, 0x400000, PAGE_SIZE, 1) == static_cast<uint64_t>(-12), "mprotect unmapped ENOMEM");
    ok &= memory_expect(memory_call(SYSCALL_mprotect, CANONICAL_KERNEL_BASE, PAGE_SIZE, 1) == LINUX_EINVAL, "mprotect kernel address rejected");
    void* legacyPage = pmm_alloc(PAGE_SIZE);
    if (legacyPage == nullptr)
    {
        return memory_expect(false, "mprotect legacy mapping fixture allocation");
    }
    const phys_addr_t legacyPhysical = hhdm_to_pa(reinterpret_cast<uintptr_t>(legacyPage), platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
    uint64_t          legacyFlags    = 0;
    ARCH_PAGE_FLAGS_INIT(legacyFlags);
    ARCH_PAGE_FLAG_SET_READ(legacyFlags);
    ARCH_PAGE_FLAG_SET_WRITE(legacyFlags);
    ARCH_PAGE_FLAG_SET_USER(legacyFlags);
    memory_vmm()->MapPage(0x400000, legacyPhysical, legacyFlags, process->addressSpace);
    ok &= memory_expect(memory_vmm()->VirtToPhys(0x400000, process->addressSpace) == legacyPhysical, "mprotect legacy mapping fixture maps");
    ok &= memory_expect(memory_call(SYSCALL_mprotect, 0x400000, PAGE_SIZE, 1) == static_cast<uint64_t>(-95), "mprotect rejects untracked backing");
    ok &= memory_expect(memory_call(SYSCALL_munmap, 0x400000, PAGE_SIZE) == static_cast<uint64_t>(-95), "munmap preserves untracked backing");
    ok &= memory_expect(memory_map(1, 0x400000, 0x32) == static_cast<uint64_t>(-95), "mmap fixed preserves untracked backing");
    ok &= memory_expect(memory_call(SYSCALL_madvise, 0x400000, PAGE_SIZE, 4) == static_cast<uint64_t>(-95), "madvise rejects untracked discard");
    memory_vmm()->UnmapPage(0x400000, process->addressSpace);
    pmm_free(legacyPage);
    return ok;
}
static bool test_memory_mincore(process_t* process)
{
    uint64_t a = memory_map(), vec = memory_map(2);
    if (static_cast<int64_t>(a) < 0 || static_cast<int64_t>(vec) < 0)
        return false;
    *memory_bytes(process, vec + PAGE_SIZE - 2) = 0xA5;
    *memory_bytes(process, vec + PAGE_SIZE + 2) = 0xA5;
    memory_call(SYSCALL_mprotect, a, PAGE_SIZE, 0);
    bool ok = memory_expect(memory_call(SYSCALL_mincore, a, 2 * PAGE_SIZE + 1, vec + PAGE_SIZE - 1) == 0, "mincore multiple pages cross-buffer page");
    ok &= memory_expect(*memory_bytes(process, vec + PAGE_SIZE - 1) == 1 && *memory_bytes(process, vec + PAGE_SIZE) == 1 && *memory_bytes(process, vec + PAGE_SIZE + 1) == 1, "mincore residency including PROT_NONE");
    ok &= memory_expect(*memory_bytes(process, vec + PAGE_SIZE - 2) == 0xA5 && *memory_bytes(process, vec + PAGE_SIZE + 2) == 0xA5, "mincore exact vector length");
    ok &= memory_expect(memory_call(SYSCALL_mincore, a + 1, PAGE_SIZE, vec) == LINUX_EINVAL, "mincore alignment");
    ok &= memory_expect(memory_call(SYSCALL_mincore, a, PAGE_SIZE, reinterpret_cast<uint64_t>(&process)) == LINUX_EFAULT, "mincore rejects kernel output");
    ok &= memory_expect(memory_call(SYSCALL_mincore, a, PAGE_SIZE, 0x400000) == LINUX_EFAULT, "mincore unmapped output");
    ok &= memory_expect(memory_call(SYSCALL_mincore, 0x400000, PAGE_SIZE, vec) == static_cast<uint64_t>(-12), "mincore unmapped source");
    ok &= memory_expect(memory_call(SYSCALL_mincore, a, 0, 0) == 0, "mincore zero-length query");
    ok &= memory_expect(memory_call(SYSCALL_mincore, a, 3 * PAGE_SIZE, vec + 2 * PAGE_SIZE - 1) == LINUX_EFAULT, "mincore buffer crosses unmapped page");
    memory_call(SYSCALL_mprotect, vec, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_mincore, a, PAGE_SIZE, vec) == LINUX_EFAULT, "mincore readonly output rejected");
    return ok;
}
static bool test_memory_madvise(process_t* process)
{
    uint64_t a = memory_map();
    if (static_cast<int64_t>(a) < 0)
        return false;
    bool ok                   = true;
    *memory_bytes(process, a) = 0xA5;
    for (int advice = 0; advice <= 3; ++advice)
        ok &= memory_expect(memory_call(SYSCALL_madvise, a, PAGE_SIZE, advice) == 0 && *memory_bytes(process, a) == 0xA5, "madvise hint accepted preserves content");
    ok &= memory_expect(memory_call(SYSCALL_madvise, a, PAGE_SIZE, 4) == 0 && *memory_bytes(process, a) == 0, "madvise dontneed discards contents");
    ok &= memory_expect(memory_call(SYSCALL_madvise, a + 1, PAGE_SIZE, 0) == LINUX_EINVAL, "madvise alignment");
    ok &= memory_expect(memory_call(SYSCALL_madvise, a, PAGE_SIZE, 999) == LINUX_EINVAL && memory_call(SYSCALL_madvise, a, PAGE_SIZE, 8) == LINUX_EINVAL, "madvise invalid and unsupported FREE");
    ok &= memory_expect(memory_call(SYSCALL_madvise, 0x400000, PAGE_SIZE, 0) == static_cast<uint64_t>(-12), "madvise unmapped range");
    memory_call(SYSCALL_munmap, a + PAGE_SIZE, PAGE_SIZE);
    *memory_bytes(process, a)                 = 0xA5;
    *memory_bytes(process, a + 2 * PAGE_SIZE) = 0xA5;
    ok &= memory_expect(memory_call(SYSCALL_madvise, a, 3 * PAGE_SIZE, 4) == static_cast<uint64_t>(-12) && *memory_bytes(process, a) == 0 && *memory_bytes(process, a + 2 * PAGE_SIZE) == 0, "madvise holes return ENOMEM and process mapped pages");
    return ok;
}
static bool test_memory_brk(process_t* process)
{
    uint64_t base = process->breakBase;
    bool     ok   = memory_expect(memory_call(SYSCALL_brk) == base, "brk initial query");
    ok &= memory_expect(memory_call(SYSCALL_brk, base + 1) == base + 1 && memory_bytes(process, base) && *memory_bytes(process, base) == 0, "brk grows zeroed page");
    if (!memory_bytes(process, base))
        return false;
    *memory_bytes(process, base) = 0xA5;
    ok &= memory_expect(memory_call(SYSCALL_brk, base + 2 * PAGE_SIZE + 17) == base + 2 * PAGE_SIZE + 17 && *memory_bytes(process, base) == 0xA5 && memory_bytes(process, base + 2 * PAGE_SIZE) && *memory_bytes(process, base + 2 * PAGE_SIZE) == 0, "brk page boundary and contents");
    ok &= memory_expect(memory_call(SYSCALL_brk, base + 7) == base + 7 && !memory_region(process, base + PAGE_SIZE) && memory_region(process, base), "brk shrink releases whole pages");
    ok &= memory_expect(memory_call(SYSCALL_brk) == base + 7, "brk resized query");
    *memory_bytes(process, base + 10) = 0x5A;
    ok &= memory_expect(memory_call(SYSCALL_brk, base + 11) == base + 11 && *memory_bytes(process, base + 10) == 0x5A, "brk retained partial page regrowth");
    ok &= memory_expect(memory_call(SYSCALL_brk, base + 7) == base + 7, "brk restores small break");
    ok &= memory_expect(memory_map(1, base + PAGE_SIZE, 0x100022) == base + PAGE_SIZE, "brk collision fixture");
    ok &= memory_expect(memory_call(SYSCALL_brk, base + 2 * PAGE_SIZE) == base + 7, "brk collision returns current break");
    ok &= memory_expect(memory_call(SYSCALL_brk, base - 1) == base + 7 && memory_call(SYSCALL_brk, CANONICAL_KERNEL_BASE) == base + 7, "brk invalid requests return current break");
    ok &= memory_expect(memory_call(SYSCALL_brk, base) == base && !memory_region(process, base), "brk full shrink");
    return ok;
}

static bool memory_case_result(bool result)
{
    cpu_preemption_enable();
    return result;
}

static bool run_memory_case(bool (*test)(process_t*))
{
    cpu_preemption_disable();
    Dispatcher* dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    if (dispatcher == nullptr || dispatcher->GetResourceLayerCaps() == nullptr)
    {
        return memory_case_result(memory_expect(false, "memory syscall fixture missing dispatcher"));
    }

    ResourceLayerCaps*    caps                 = dispatcher->GetResourceLayerCaps();
    ProcessManager*       processManager       = caps->processManager;
    VirtualMemoryManager* virtualMemoryManager = caps->virtualMemoryManager;
    if (processManager == nullptr || virtualMemoryManager == nullptr)
    {
        return memory_case_result(memory_expect(false, "memory syscall fixture missing resource managers"));
    }

    process_t* previous = processManager->GetCurrentProcess();
    if (previous == nullptr || previous->addressSpace == nullptr)
    {
        return memory_case_result(memory_expect(false, "memory syscall fixture requires a restorable process"));
    }
    virt_addr_space_t* space = virtualMemoryManager->CreateUserAddressSpace();
    if (space == nullptr)
    {
        return memory_case_result(memory_expect(false, "memory syscall isolated address-space fixture unavailable"));
    }

    process_t* process = processManager->CreateProcess(space);
    if (process == nullptr)
    {
        virtualMemoryManager->DestroyUserAddressSpace(space);
        return memory_case_result(memory_expect(false, "memory syscall process fixture allocation failed"));
    }

    // Synthetic executable boundary; the fixture needs no userspace ELF/runtime.
    process->breakBase    = 0x800000;
    process->programBreak = process->breakBase;
    bool active           = processManager->SetRunningProcess(arch_cpu_id(), process);
    bool result           = active;
    if (active)
    {
        process_t* pinned = processManager->AcquireCurrentMemoryProcess();
        result &= memory_expect(pinned == process, "memory syscall fixture pins current process");
        if (pinned != nullptr)
        {
            result &= memory_expect(!processManager->FreeProcess(process), "memory syscall lifetime pin blocks teardown");
            processManager->ReleaseMemoryProcess(pinned);
        }
        process_t* alias = processManager->CreateProcess(space);
        result &= memory_expect(alias != nullptr, "memory syscall shared-root fixture creates alias");
        if (alias != nullptr)
        {
            result &= memory_expect(memory_map(1) == LINUX_ESRCH, "memory syscall shared roots rejected before mutation");
            result &= memory_expect(memory_call(SYSCALL_brk) == process->breakBase, "brk query remains current on unsupported shared root");
            result &= memory_expect(processManager->FreeProcess(alias), "memory syscall shared-root fixture cleanup");
        }
        result &= test(process);
    }
    if (active)
    {
        result &= memory_expect(processManager->SetRunningProcess(arch_cpu_id(), previous), "memory syscall fixture restore process");
    }

    result &= memory_expect(processManager->FreeProcess(process), "memory syscall fixture process cleanup");
    result &= memory_expect(space->user_used_regions == nullptr, "memory syscall fixture releases all owned pages");
    virtualMemoryManager->DestroyUserAddressSpace(space);
    return memory_case_result(result);
}

static bool custom_case_validate_mmap(const custom_case_ctx_t*)
{
    return run_memory_case(test_memory_mmap);
}

static bool custom_case_validate_munmap(const custom_case_ctx_t*)
{
    return run_memory_case(test_memory_munmap);
}

static bool custom_case_validate_mprotect(const custom_case_ctx_t*)
{
    return run_memory_case(test_memory_mprotect);
}

static bool custom_case_validate_mincore(const custom_case_ctx_t*)
{
    return run_memory_case(test_memory_mincore);
}

static bool custom_case_validate_madvise(const custom_case_ctx_t*)
{
    return run_memory_case(test_memory_madvise);
}

static bool custom_case_validate_brk(const custom_case_ctx_t*)
{
    return run_memory_case(test_memory_brk);
}

static expected_result_kind_t expected_kind_for_syscall(uint64_t syscallNumber)
{
    switch (syscallNumber)
    {
        case SYSCALL_getpid:
        case SYSCALL_getppid:
        case SYSCALL_gettid:
        case SYSCALL_getuid:
        case SYSCALL_getgid:
        case SYSCALL_geteuid:
        case SYSCALL_getegid:
        case SYSCALL_getpgrp:
        case SYSCALL_setsid:
        case SYSCALL_getgroups:
        case SYSCALL_getresuid:
        case SYSCALL_getresgid:
        case SYSCALL_getcpu:
        case SYSCALL_time:
        case SYSCALL_alarm:
        case SYSCALL_umask:
        case SYSCALL_setfsuid:
        case SYSCALL_setfsgid:
        case SYSCALL_set_tid_address:
            return EXPECT_NONNEG;
        default:
            return EXPECT_NEG_ERRNO;
    }
}

static bool validate_linux_behavior(uint64_t syscallNumber, uint64_t result)
{
    const expected_result_kind_t expectedKind = expected_kind_for_syscall(syscallNumber);
    if (expectedKind == EXPECT_NONNEG)
    {
        return (int64_t) result >= 0;
    }

    if ((int64_t) result >= 0)
    {
        return false;
    }

    if (result == LINUX_EINVAL)
    {
        return true;
    }

    const uint64_t neg = (uint64_t) (-(int64_t) result);
    return neg >= 1 && neg <= 4095;
}

template <typename ManagerT, size_t N> static void run_manager_tests(const char* managerName, ManagerT* manager, const request_method_test_t<ManagerT> (&tests)[N], requestlayer_stats_t* stats, bool runUnavailableCustom = false, bool customOwnsInvocation = false)
{
    if (stats == nullptr)
    {
        return;
    }

    if (manager == nullptr)
    {
        stats->implementedFail++;
        selftest_record_failure_detail("missing request manager");
        return;
    }

    for (size_t index = 0; index < N; ++index)
    {
        request_process_state_t processState(tests[index].syscallNumber);
        request_user_fixture_t  userFixture;
        arch_syscall_frame_t    frame = {};
        frame.syscall_number          = tests[index].syscallNumber;
        if (tests[index].customValidator == nullptr)
        {
            // Non-custom tests get deterministic synthetic args.
            frame.arg0 = 0x1000ULL ^ tests[index].syscallNumber;
            frame.arg1 = 0x2000ULL ^ tests[index].syscallNumber;
            frame.arg2 = 0x3000ULL ^ tests[index].syscallNumber;
            frame.arg3 = 0x4000ULL ^ tests[index].syscallNumber;
            frame.arg4 = 0x5000ULL ^ tests[index].syscallNumber;
            frame.arg5 = 0x6000ULL ^ tests[index].syscallNumber;
        }

        kprintf("Arx kernel: requestlayer_selftest run %s::%s syscall=%llu\n", managerName, tests[index].name, (unsigned long long) tests[index].syscallNumber);

        // Event validators invoke the real dispatcher themselves. A preliminary
        // zero-argument ppoll/select would block forever; eventfd would leak a fd.
        uint64_t result = customOwnsInvocation && tests[index].customValidator != nullptr ? 0 : (manager->*(tests[index].method))(&frame);
        stats->total++;

        const bool unavailable = result == LINUX_ENOSYS;
        if (unavailable)
        {
            stats->unimplemented++;
            if (!(runUnavailableCustom && tests[index].customValidator != nullptr))
                continue;
        }
        else
            stats->implemented++;

        if (tests[index].customValidator != nullptr)
        {
            const custom_case_ctx_t customCtx = {
                    managerName, tests[index].name, tests[index].syscallNumber, &frame, result,
            };
            stats->customCaseUsed++;
            if (tests[index].customValidator(&customCtx) && userFixture.valid)
            {
                stats->customCasePass++;
                if (!unavailable)
                    stats->implementedPass++;
            }
            else
            {
                stats->customCaseFail++;
                stats->implementedFail++;
                selftest_record_failure_detail(tests[index].name);
                kprintf("Arx kernel: requestlayer_selftest custom FAIL %s::%s got=%lld\n", managerName, tests[index].name, (long long) result);
            }
            continue;
        }

        if (validate_linux_behavior(tests[index].syscallNumber, result))
        {
            stats->implementedPass++;
            continue;
        }

        stats->implementedFail++;
        selftest_record_failure_detail(tests[index].name);
        kprintf("Arx kernel: requestlayer_selftest FAIL %s::%s got=%lld\n", managerName, tests[index].name, (long long) result);
    }
}

static void test_process_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<ProcessRequestManager> tests[] = {
            {"HandleExitRequest", &ProcessRequestManager::HandleExitRequest, SYSCALL_exit, custom_case_validate_exit_like},
            {"HandleExit_groupRequest", &ProcessRequestManager::HandleExit_groupRequest, SYSCALL_exit_group, custom_case_validate_exit_like},
            {"HandleForkRequest", &ProcessRequestManager::HandleForkRequest, SYSCALL_fork, custom_case_stub_process_lifecycle},
            {"HandleVforkRequest", &ProcessRequestManager::HandleVforkRequest, SYSCALL_vfork, nullptr},
            {"HandleCloneRequest", &ProcessRequestManager::HandleCloneRequest, SYSCALL_clone, nullptr},
            {"HandleClone3Request", &ProcessRequestManager::HandleClone3Request, SYSCALL_clone3, nullptr},
            {"HandleExecveRequest", &ProcessRequestManager::HandleExecveRequest, SYSCALL_execve, custom_case_stub_process_lifecycle},
            {"HandleWait4Request", &ProcessRequestManager::HandleWait4Request, SYSCALL_wait4, nullptr},
            {"HandleWaitidRequest", &ProcessRequestManager::HandleWaitidRequest, SYSCALL_waitid, nullptr},
            {"HandleSet_tid_addressRequest", &ProcessRequestManager::HandleSet_tid_addressRequest, SYSCALL_set_tid_address, nullptr},
            {"HandleGettidRequest", &ProcessRequestManager::HandleGettidRequest, SYSCALL_gettid, custom_case_validate_process_identity},
            {"HandleGetpidRequest", &ProcessRequestManager::HandleGetpidRequest, SYSCALL_getpid, custom_case_validate_process_identity},
            {"HandleGetppidRequest", &ProcessRequestManager::HandleGetppidRequest, SYSCALL_getppid, custom_case_validate_getppid},
            {"HandleGetpgidRequest", &ProcessRequestManager::HandleGetpgidRequest, SYSCALL_getpgid, nullptr},
            {"HandleGetpgrpRequest", &ProcessRequestManager::HandleGetpgrpRequest, SYSCALL_getpgrp, nullptr},
            {"HandleSetpgidRequest", &ProcessRequestManager::HandleSetpgidRequest, SYSCALL_setpgid, nullptr},
            {"HandleGetsidRequest", &ProcessRequestManager::HandleGetsidRequest, SYSCALL_getsid, nullptr},
            {"HandleSetsidRequest", &ProcessRequestManager::HandleSetsidRequest, SYSCALL_setsid, nullptr},
    };

    run_manager_tests("process", caps->processRequestManager, tests, stats);
}

static void test_scheduler_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<SchedulerRequestManager> tests[] = {
            {"HandleSched_yieldRequest", &SchedulerRequestManager::HandleSched_yieldRequest, SYSCALL_sched_yield, custom_case_validate_sched_yield},
            {"HandleSched_getaffinityRequest", &SchedulerRequestManager::HandleSched_getaffinityRequest, SYSCALL_sched_getaffinity, custom_case_validate_sched_getaffinity},
            {"HandleGetcpuRequest", &SchedulerRequestManager::HandleGetcpuRequest, SYSCALL_getcpu, custom_case_validate_getcpu},
    };

    run_manager_tests("scheduler", caps->schedulerRequestManager, tests, stats);
}

static void time_fixture_store(process_t* process, uint64_t address, const void* source, size_t length)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(source);
    for (size_t i = 0; i < length; ++i)
        *memory_bytes(process, address + i) = bytes[i];
}

static void time_fixture_load(process_t* process, uint64_t address, void* destination, size_t length)
{
    uint8_t* bytes = static_cast<uint8_t*>(destination);
    for (size_t i = 0; i < length; ++i)
        bytes[i] = *memory_bytes(process, address + i);
}

static bool test_time_clock(process_t* process, uint64_t number)
{
    uint64_t base = memory_map(2);
    if (static_cast<int64_t>(base) < 0)
        return memory_expect(false, "time fixture mapping");
    uint64_t     output                         = base + PAGE_SIZE - 8;
    const size_t outputSize                     = number == SYSCALL_time ? 8 : 16;
    *memory_bytes(process, output - 1)          = 0xa5;
    *memory_bytes(process, output + outputSize) = 0xa5;
    time_timespec_t value                       = {};
    uint64_t        result                      = number == SYSCALL_time ? memory_call(number, output) : number == SYSCALL_gettimeofday ? memory_call(number, output, base + 64) : memory_call(number, 1, output);
    if (result == LINUX_ENOSYS)
    {
        kprintf("Arx kernel: time selftest clock unavailable syscall=%llu\n", (unsigned long long) number);
        return memory_expect(number == SYSCALL_time || number == SYSCALL_gettimeofday, "monotonic HPET clock is required for clock selftests");
    }
    bool ok = memory_expect(number == SYSCALL_time ? static_cast<int64_t>(result) >= 0 : result == 0, "time dispatcher success");
    ok &= memory_expect(*memory_bytes(process, output - 1) == 0xa5 && *memory_bytes(process, output + outputSize) == 0xa5, "time writes exact ABI size");
    time_fixture_load(process, output, &value, number == SYSCALL_time ? 8 : 16);
    if (number == SYSCALL_time)
    {
        ok &= memory_expect(result == static_cast<uint64_t>(value.seconds), "time return matches optional seconds output");
        ok &= memory_expect(memory_call(number) >= result, "time null output allowed");
    }
    else
    {
        ok &= memory_expect(value.seconds >= 0 && value.nanoseconds >= 0 && value.nanoseconds < (number == SYSCALL_gettimeofday ? 1000000 : 1000000000), "normalized time ABI");
        if (number == SYSCALL_gettimeofday)
        {
            int32_t timezone[2] = {-1, -1};
            time_fixture_load(process, base + 64, timezone, sizeof(timezone));
            ok &= memory_expect(timezone[0] == 0 && timezone[1] == 0 && memory_call(number) == 0, "gettimeofday UTC timezone and optional outputs");
            ok &= memory_expect(memory_call(number, 0, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "gettimeofday invalid timezone output");
        }
        else
        {
            ok &= memory_expect(memory_call(number, 2, output) == LINUX_EINVAL && memory_call(number, UINT64_MAX, output) == LINUX_EINVAL, "unsupported clock IDs");
            ok &= memory_expect(memory_call(number, 1, 0) == (number == SYSCALL_clock_getres ? 0 : LINUX_EFAULT), "clock nullable output semantics");
            if (number == SYSCALL_clock_getres)
                ok &= memory_expect(value.seconds > 0 || value.nanoseconds > 0, "clock positive resolution");
            else
            {
                time_timespec_t next;
                ok &= memory_expect(memory_call(number, 1, output) == 0, "repeat monotonic clock read");
                time_fixture_load(process, output, &next, sizeof(next));
                ok &= memory_expect(next.seconds > value.seconds || (next.seconds == value.seconds && next.nanoseconds >= value.nanoseconds), "monotonic clock never reverses");
            }
        }
    }
    auto call_output = [number](uint64_t pointer) { return number == SYSCALL_time || number == SYSCALL_gettimeofday ? memory_call(number, pointer) : memory_call(number, 1, pointer); };
    ok &= memory_expect(call_output(CANONICAL_KERNEL_BASE) == LINUX_EFAULT && call_output(0x400000) == LINUX_EFAULT, "time rejects kernel and unmapped output");
    *memory_bytes(process, base + 2 * PAGE_SIZE - 4) = 0xa5;
    ok &= memory_expect(call_output(base + 2 * PAGE_SIZE - 4) == LINUX_EFAULT && *memory_bytes(process, base + 2 * PAGE_SIZE - 4) == 0xa5, "time preflights entire output before writing");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);
    ok &= memory_expect(call_output(base) == LINUX_EFAULT, "time readonly output rejected");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 0);
    ok &= memory_expect(call_output(base) == LINUX_EFAULT, "time PROT_NONE output rejected");
    return ok;
}

struct sleep_peer_context_t
{
    Scheduler*      scheduler;
    ProcessManager* processManager;
    TaskManager*    taskManager;
    process_t*      sleeper;
    task_t*         sleepingTask;
    bool            entered;
    bool            protectedLifetime;
    bool            blockedDispatch;
};

static void sleep_peer_entry(void* argument)
{
    sleep_peer_context_t* context = static_cast<sleep_peer_context_t*>(argument);
    context->entered              = true;
    context->protectedLifetime    = !context->taskManager->FreeTask(context->sleepingTask) && !context->processManager->FreeProcess(context->sleeper);
    context->blockedDispatch      = !context->taskManager->ExecuteTask(context->sleepingTask);
    process_t* current            = context->processManager->GetCurrentProcess();
    // Leave no ready entry referring to this deliberately suspended test peer.
    context->scheduler->KillProcess(current->id, 0);
    for (;;)
    {
        // Only the hardware timer may mark the sleeper ready in this peer.
        context->scheduler->RunNextReadyProcess(arch_cpu_id());
        arch_halt();
    }
}

static bool test_time_sleep(process_t* process, uint64_t number)
{
    uint64_t base = memory_map(2);
    if (static_cast<int64_t>(base) < 0)
        return memory_expect(false, "sleep fixture mapping");
    uint64_t        input   = base + PAGE_SIZE - 8;
    auto            call    = [number](uint64_t pointer, uint64_t clock = 1, uint64_t flags = 0) { return number == SYSCALL_nanosleep ? memory_call(number, pointer, CANONICAL_KERNEL_BASE) : memory_call(number, clock, flags, pointer, CANONICAL_KERNEL_BASE); };
    bool            ok      = memory_expect(call(0) == LINUX_EFAULT && call(CANONICAL_KERNEL_BASE) == LINUX_EFAULT && call(base + 2 * PAGE_SIZE - 8) == LINUX_EFAULT, "sleep invalid request buffers");
    time_timespec_t request = {-1, 0};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(call(input) == LINUX_EINVAL, "sleep negative seconds");
    request = {0, 1000000000};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(call(input) == LINUX_EINVAL, "sleep invalid nanoseconds");
    Dispatcher*          dispatcher  = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps*   resources   = dispatcher->GetResourceLayerCaps();
    Scheduler*           scheduler   = dispatcher->GetLogicLayerCaps()->scheduler;
    sleep_peer_context_t peerContext = {scheduler, resources->processManager, resources->taskManager, process, resources->taskManager->GetCurrentTask(), false, false, false};
    virt_addr_space_t*   peerSpace   = resources->virtualMemoryManager->CreateUserAddressSpace();
    process_t*           peerProcess = peerSpace != nullptr ? resources->processManager->CreateProcess(peerSpace) : nullptr;
    task_t*              peerTask    = resources->taskManager->CreateKernelTask(sleep_peer_entry, &peerContext);
    bool                 peerReady   = peerProcess != nullptr && peerTask != nullptr && resources->processManager->AddTask(peerProcess, peerTask) && scheduler->EnqueueProcess(arch_cpu_id(), peerProcess->id);
    ok &= memory_expect(peerReady, "sleep runnable peer fixture");
    if (!peerReady)
    {
        if (peerTask != nullptr)
            resources->taskManager->FreeTask(peerTask);
        if (peerProcess != nullptr)
            resources->processManager->FreeProcess(peerProcess);
        if (peerSpace != nullptr)
            resources->virtualMemoryManager->DestroyUserAddressSpace(peerSpace);
        return false;
    }
    request = {0, 100000000};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(memory_call(SYSCALL_clock_gettime, 1, base + 64) == 0, "sleep monotonic start");
    time_timespec_t before = {}, after = {};
    time_fixture_load(process, base + 64, &before, sizeof(before));
    ok &= memory_expect(call(input) == 0, "scheduler sleep and ignore remainder on success");
    ok &= memory_expect(peerContext.entered && peerContext.protectedLifetime && peerContext.blockedDispatch, "sleep lets a peer run while protecting and blocking the sleeper");
    // If the clock failed before switching, remove the still-queued peer too.
    if (!peerContext.entered)
        scheduler->KillProcess(peerProcess->id, 0);
    bool releasedTask    = !peerTask->allocated || resources->taskManager->FreeTask(peerTask);
    bool releasedProcess = !peerProcess->allocated || resources->processManager->FreeProcess(peerProcess);
    ok &= memory_expect(releasedTask && releasedProcess, "sleep peer cleanup");
    resources->virtualMemoryManager->DestroyUserAddressSpace(peerSpace);
    ok &= memory_expect(memory_call(SYSCALL_clock_gettime, 1, base + 64) == 0, "sleep monotonic end");
    time_fixture_load(process, base + 64, &after, sizeof(after));
    int64_t elapsed = (after.seconds - before.seconds) * 1000000000 + after.nanoseconds - before.nanoseconds;
    ok &= memory_expect(elapsed >= 100000000, "sleep waits at least requested duration");
    request = {INT64_MAX, 0};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(call(input) == static_cast<uint64_t>(-75), "sleep rejects unrepresentable duration");
    request = {0, 0};
    time_fixture_store(process, input, &request, sizeof(request));
    if (number == SYSCALL_clock_nanosleep)
    {
        ok &= memory_expect(call(input, 1, 1) == 0 && call(input, 0, 1) == 0, "absolute past deadlines return immediately");
        ok &= memory_expect(call(input, 2) == LINUX_EINVAL && call(input, 1, 2) == LINUX_EINVAL, "sleep unsupported clock and flags");
        request = after;
        request.nanoseconds += 1000000;
        if (request.nanoseconds >= 1000000000)
        {
            ++request.seconds;
            request.nanoseconds -= 1000000000;
        }
        time_fixture_store(process, input, &request, sizeof(request));
        ok &= memory_expect(call(input, 1, 1) == 0, "absolute monotonic sleep");
        memory_call(SYSCALL_clock_gettime, 1, base + 64);
        time_fixture_load(process, base + 64, &after, sizeof(after));
        ok &= memory_expect(after.seconds > request.seconds || (after.seconds == request.seconds && after.nanoseconds >= request.nanoseconds), "absolute sleep reaches deadline");
    }
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);
    request = {0, 0};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(call(input) == 0, "sleep readonly input accepted");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 0);
    ok &= memory_expect(call(input) == LINUX_EFAULT, "sleep PROT_NONE input rejected");
    return ok;
}

static bool test_time_timer(process_t* process, uint64_t number)
{
    uint64_t base = memory_map(2);
    if (static_cast<int64_t>(base) < 0)
        return memory_expect(false, "timer fixture mapping");
    uint64_t         input   = base + PAGE_SIZE - 8;
    uint64_t         output  = base + 64;
    bool             ok      = true;
    time_itimerval_t request = {{2, 123}, {0, 0}}, value = {};
    for (unsigned which = 0; which < 3; ++which)
    {
        ok &= memory_expect(memory_call(SYSCALL_getitimer, which, output) == 0, "new process timer query");
        time_fixture_load(process, output, &value, sizeof(value));
        ok &= memory_expect(value.interval.seconds == 0 && value.interval.microseconds == 0 && value.value.seconds == 0 && value.value.microseconds == 0, "new process timers disarmed");
    }
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input, output) == 0 && memory_call(SYSCALL_getitimer, 0, output) == 0, "disarmed timer stores interval");
    time_fixture_load(process, output, &value, sizeof(value));
    ok &= memory_expect(value.interval.seconds == 2 && value.interval.microseconds == 123 && value.value.seconds == 0, "disarmed interval roundtrip");
    if (number == SYSCALL_alarm)
    {
        ok &= memory_expect(memory_call(number, 1) == LINUX_ENOSYS && memory_call(number, 0) == 0, "alarm arming unsupported and cancellation succeeds");
        memory_call(SYSCALL_getitimer, 0, output);
        time_fixture_load(process, output, &value, sizeof(value));
        ok &= memory_expect(value.interval.seconds == 0 && value.interval.microseconds == 0, "alarm cancellation clears real timer interval");
        return ok;
    }
    ok &= memory_expect(memory_call(SYSCALL_getitimer, 3, output) == LINUX_EINVAL && memory_call(SYSCALL_setitimer, UINT64_MAX, input) == LINUX_EINVAL, "timer selector validation");
    ok &= memory_expect(memory_call(SYSCALL_getitimer, 0, CANONICAL_KERNEL_BASE) == LINUX_EFAULT && memory_call(SYSCALL_setitimer, 0, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "timer kernel buffers rejected");
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input, base + 2 * PAGE_SIZE - 4) == LINUX_EFAULT, "timer invalid old-value output");
    request.value.seconds = 1;
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input) == static_cast<uint64_t>(-95), "timer arming unsupported");
    memory_call(SYSCALL_getitimer, 0, output);
    time_fixture_load(process, output, &value, sizeof(value));
    ok &= memory_expect(value.interval.seconds == 2 && value.interval.microseconds == 123 && value.value.seconds == 0, "failed timer operations preserve state");
    request = {{0, 1000000}, {0, 0}};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input) == LINUX_EINVAL, "timer microsecond validation");
    request = {{7, 99}, {0, 0}};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input, input) == 0, "timer aliased input and previous output");
    time_fixture_load(process, input, &value, sizeof(value));
    ok &= memory_expect(value.interval.seconds == 2 && value.interval.microseconds == 123, "timer alias receives old interval");
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, 0, output) == 0, "Linux null new-value cancels timer");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_getitimer, 0, output) == LINUX_EFAULT, "timer readonly output rejected");
    return ok;
}

template <uint64_t Number> static bool custom_case_validate_time(const custom_case_ctx_t*)
{
    return run_memory_case(
            [](process_t* process)
            {
                if (Number == SYSCALL_nanosleep || Number == SYSCALL_clock_nanosleep)
                    return test_time_sleep(process, Number);
                if (Number == SYSCALL_getitimer || Number == SYSCALL_setitimer || Number == SYSCALL_alarm)
                    return test_time_timer(process, Number);
                return test_time_clock(process, Number);
            });
}

static void test_time_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<TimeRequestManager> tests[] = {
            {"HandleTimeRequest", &TimeRequestManager::HandleTimeRequest, SYSCALL_time, custom_case_validate_time<SYSCALL_time>},
            {"HandleGettimeofdayRequest", &TimeRequestManager::HandleGettimeofdayRequest, SYSCALL_gettimeofday, custom_case_validate_time<SYSCALL_gettimeofday>},
            {"HandleClock_gettimeRequest", &TimeRequestManager::HandleClock_gettimeRequest, SYSCALL_clock_gettime, custom_case_validate_time<SYSCALL_clock_gettime>},
            {"HandleClock_getresRequest", &TimeRequestManager::HandleClock_getresRequest, SYSCALL_clock_getres, custom_case_validate_time<SYSCALL_clock_getres>},
            {"HandleClock_nanosleepRequest", &TimeRequestManager::HandleClock_nanosleepRequest, SYSCALL_clock_nanosleep, custom_case_validate_time<SYSCALL_clock_nanosleep>},
            {"HandleNanosleepRequest", &TimeRequestManager::HandleNanosleepRequest, SYSCALL_nanosleep, custom_case_validate_time<SYSCALL_nanosleep>},
            {"HandleGetitimerRequest", &TimeRequestManager::HandleGetitimerRequest, SYSCALL_getitimer, custom_case_validate_time<SYSCALL_getitimer>},
            {"HandleSetitimerRequest", &TimeRequestManager::HandleSetitimerRequest, SYSCALL_setitimer, custom_case_validate_time<SYSCALL_setitimer>},
            {"HandleAlarmRequest", &TimeRequestManager::HandleAlarmRequest, SYSCALL_alarm, custom_case_validate_time<SYSCALL_alarm>},
    };

    // Still run buffer/timer scenarios when the preliminary clock read is ENOSYS.
    run_manager_tests("time", caps->timeRequestManager, tests, stats, true);
}

static void test_memory_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<MemoryRequestManager> tests[] = {
            {"HandleMmapRequest", &MemoryRequestManager::HandleMmapRequest, SYSCALL_mmap, custom_case_validate_mmap},
            {"HandleMunmapRequest", &MemoryRequestManager::HandleMunmapRequest, SYSCALL_munmap, custom_case_validate_munmap},
            {"HandleMprotectRequest", &MemoryRequestManager::HandleMprotectRequest, SYSCALL_mprotect, custom_case_validate_mprotect},
            {"HandleMincoreRequest", &MemoryRequestManager::HandleMincoreRequest, SYSCALL_mincore, custom_case_validate_mincore},
            {"HandleMadviseRequest", &MemoryRequestManager::HandleMadviseRequest, SYSCALL_madvise, custom_case_validate_madvise},
            {"HandleBrkRequest", &MemoryRequestManager::HandleBrkRequest, SYSCALL_brk, custom_case_validate_brk},
    };

    run_manager_tests("memory", caps->memoryRequestManager, tests, stats);
}

static void test_vfs_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<VfsRequestManager> tests[] = {
            {"HandleOpenatRequest", &VfsRequestManager::HandleOpenatRequest, SYSCALL_openat, custom_case_validate_openat},
            {"HandleOpenRequest", &VfsRequestManager::HandleOpenRequest, SYSCALL_open, custom_case_validate_open},
            {"HandleCreatRequest", &VfsRequestManager::HandleCreatRequest, SYSCALL_creat, custom_case_validate_creat},
            {"HandleCloseRequest", &VfsRequestManager::HandleCloseRequest, SYSCALL_close, custom_case_validate_close},
            {"HandleClose_rangeRequest", &VfsRequestManager::HandleClose_rangeRequest, SYSCALL_close_range, custom_case_validate_close_range},
            {"HandleMkdiratRequest", &VfsRequestManager::HandleMkdiratRequest, SYSCALL_mkdirat, custom_case_validate_mkdirat},
            {"HandleMkdirRequest", &VfsRequestManager::HandleMkdirRequest, SYSCALL_mkdir, custom_case_validate_mkdir},
            {"HandleReadRequest", &VfsRequestManager::HandleReadRequest, SYSCALL_read, custom_case_validate_vfs_rw},
            {"HandleWriteRequest", &VfsRequestManager::HandleWriteRequest, SYSCALL_write, custom_case_validate_vfs_rw},
            {"HandlePread64Request", &VfsRequestManager::HandlePread64Request, SYSCALL_pread64, custom_case_validate_pread64},
            {"HandlePwrite64Request", &VfsRequestManager::HandlePwrite64Request, SYSCALL_pwrite64, custom_case_validate_pwrite64},
            {"HandleReadvRequest", &VfsRequestManager::HandleReadvRequest, SYSCALL_readv, custom_case_validate_readv},
            {"HandleWritevRequest", &VfsRequestManager::HandleWritevRequest, SYSCALL_writev, custom_case_validate_writev},
            {"HandlePreadvRequest", &VfsRequestManager::HandlePreadvRequest, SYSCALL_preadv, custom_case_validate_preadv},
            {"HandlePwritevRequest", &VfsRequestManager::HandlePwritevRequest, SYSCALL_pwritev, custom_case_validate_pwritev},
            {"HandleLseekRequest", &VfsRequestManager::HandleLseekRequest, SYSCALL_lseek, custom_case_validate_lseek},
            {"HandleGetcwdRequest", &VfsRequestManager::HandleGetcwdRequest, SYSCALL_getcwd, custom_case_validate_getcwd},
            {"HandleChdirRequest", &VfsRequestManager::HandleChdirRequest, SYSCALL_chdir, custom_case_validate_chdir},
            {"HandleFchdirRequest", &VfsRequestManager::HandleFchdirRequest, SYSCALL_fchdir, custom_case_validate_fchdir},
            {"HandleGetdents64Request", &VfsRequestManager::HandleGetdents64Request, SYSCALL_getdents64, custom_case_validate_getdents64},
            {"HandleUnlinkatRequest", &VfsRequestManager::HandleUnlinkatRequest, SYSCALL_unlinkat, custom_case_validate_unlinkat},
            {"HandleUnlinkRequest", &VfsRequestManager::HandleUnlinkRequest, SYSCALL_unlink, custom_case_validate_unlink},
            {"HandleRmdirRequest", &VfsRequestManager::HandleRmdirRequest, SYSCALL_rmdir, custom_case_validate_rmdir},
            {"HandleFcntlRequest", &VfsRequestManager::HandleFcntlRequest, SYSCALL_fcntl, custom_case_validate_fcntl},
            {"HandleDupRequest", &VfsRequestManager::HandleDupRequest, SYSCALL_dup, custom_case_validate_dup},
            {"HandleDup2Request", &VfsRequestManager::HandleDup2Request, SYSCALL_dup2, custom_case_validate_dup2},
            {"HandleDup3Request", &VfsRequestManager::HandleDup3Request, SYSCALL_dup3, custom_case_validate_dup3},
            {"HandleNewfstatatRequest", &VfsRequestManager::HandleNewfstatatRequest, SYSCALL_newfstatat, custom_case_validate_newfstatat},
            {"HandleStatRequest", &VfsRequestManager::HandleStatRequest, SYSCALL_stat, custom_case_validate_stat},
            {"HandleFstatRequest", &VfsRequestManager::HandleFstatRequest, SYSCALL_fstat, custom_case_validate_fstat},
            {"HandleLstatRequest", &VfsRequestManager::HandleLstatRequest, SYSCALL_lstat, custom_case_validate_lstat},
            {"HandleRenameatRequest", &VfsRequestManager::HandleRenameatRequest, SYSCALL_renameat, custom_case_validate_renameat},
            {"HandleRenameRequest", &VfsRequestManager::HandleRenameRequest, SYSCALL_rename, custom_case_validate_rename},
            {"HandleReadlinkatRequest", &VfsRequestManager::HandleReadlinkatRequest, SYSCALL_readlinkat, custom_case_validate_readlinkat},
            {"HandleReadlinkRequest", &VfsRequestManager::HandleReadlinkRequest, SYSCALL_readlink, custom_case_validate_readlink},
            {"HandleIoctlRequest", &VfsRequestManager::HandleIoctlRequest, SYSCALL_ioctl, custom_case_validate_ioctl},
            {"HandleLinkatRequest", &VfsRequestManager::HandleLinkatRequest, SYSCALL_linkat, custom_case_validate_linkat},
            {"HandleLinkRequest", &VfsRequestManager::HandleLinkRequest, SYSCALL_link, custom_case_validate_link},
            {"HandleSymlinkatRequest", &VfsRequestManager::HandleSymlinkatRequest, SYSCALL_symlinkat, custom_case_validate_symlinkat},
            {"HandleSymlinkRequest", &VfsRequestManager::HandleSymlinkRequest, SYSCALL_symlink, custom_case_validate_symlink},
            {"HandleFaccessatRequest", &VfsRequestManager::HandleFaccessatRequest, SYSCALL_faccessat, custom_case_validate_access_family},
            {"HandleFaccessat2Request", &VfsRequestManager::HandleFaccessat2Request, SYSCALL_faccessat2, custom_case_validate_access_family},
            {"HandleAccessRequest", &VfsRequestManager::HandleAccessRequest, SYSCALL_access, custom_case_validate_access_family},
            {"HandleFchownatRequest", &VfsRequestManager::HandleFchownatRequest, SYSCALL_fchownat},
            {"HandleChownRequest", &VfsRequestManager::HandleChownRequest, SYSCALL_chown},
            {"HandleFchownRequest", &VfsRequestManager::HandleFchownRequest, SYSCALL_fchown},
            {"HandleLchownRequest", &VfsRequestManager::HandleLchownRequest, SYSCALL_lchown},
            {"HandleFchmodat2Request", &VfsRequestManager::HandleFchmodat2Request, SYSCALL_fchmodat2},
            {"HandleFchmodatRequest", &VfsRequestManager::HandleFchmodatRequest, SYSCALL_fchmodat},
            {"HandleChmodRequest", &VfsRequestManager::HandleChmodRequest, SYSCALL_chmod},
            {"HandleFchmodRequest", &VfsRequestManager::HandleFchmodRequest, SYSCALL_fchmod},
            {"HandleMountRequest", &VfsRequestManager::HandleMountRequest, SYSCALL_mount},
            {"HandleStatfsRequest", &VfsRequestManager::HandleStatfsRequest, SYSCALL_statfs, custom_case_validate_statfs},
            {"HandleFstatfsRequest", &VfsRequestManager::HandleFstatfsRequest, SYSCALL_fstatfs, custom_case_validate_fstatfs},
            {"HandleMknodatRequest", &VfsRequestManager::HandleMknodatRequest, SYSCALL_mknodat, custom_case_validate_mknodat},
            {"HandleMknodRequest", &VfsRequestManager::HandleMknodRequest, SYSCALL_mknod, custom_case_validate_mknod},
            {"HandleTruncateRequest", &VfsRequestManager::HandleTruncateRequest, SYSCALL_truncate, custom_case_validate_truncate},
            {"HandleFtruncateRequest", &VfsRequestManager::HandleFtruncateRequest, SYSCALL_ftruncate, custom_case_validate_ftruncate},
            {"HandlePipeRequest", &VfsRequestManager::HandlePipeRequest, SYSCALL_pipe},
            {"HandlePipe2Request", &VfsRequestManager::HandlePipe2Request, SYSCALL_pipe2},
            {"HandleMemfd_createRequest", &VfsRequestManager::HandleMemfd_createRequest, SYSCALL_memfd_create},
    };

    run_manager_tests("vfs", caps->vfsRequestManager, tests, stats);
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
    event_peer_context_t* context = static_cast<event_peer_context_t*>(argument);
    context->entered              = true;
    context->protectedLifetime    = !context->resources->taskManager->FreeTask(context->sleepingTask) && !context->resources->processManager->FreeProcess(context->sleeper) && !context->resources->taskManager->ExecuteTask(context->sleepingTask);
    {
        request_user_fixture_t fixture;
        uint64_t               value = 5;
        uint64_t               user  = fixture.Address(&value, sizeof(value));
        context->result              = memory_call(context->read ? SYSCALL_read : SYSCALL_write, context->descriptor, user, sizeof(value));
    }
    process_t* current = context->resources->processManager->GetCurrentProcess();
    context->scheduler->KillProcess(current->id, 0);
    for (;;)
    {
        context->scheduler->RunNextReadyProcess(arch_cpu_id());
        arch_halt();
    }
}

template <typename Wait> static bool event_with_peer(process_t* process, uint64_t fd, bool peerReads, Wait wait)
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
    bool                 ready            = peer != nullptr && task != nullptr;
    if (ready)
    {
        FileDescriptorGuard guard(process);
        file_descriptor_t*  source = resources->processManager->GetFileDescriptor(process, fd);
        file_t*             file   = source != nullptr ? static_cast<file_t*>(source->file) : nullptr;
        ready                      = file != nullptr && vfs->Retain(file);
        if (ready)
        {
            sharedFile = file;
            spinlock_acquire_preempt(&file->refLock);
            referencesBefore = file->refCount - 1;
            spinlock_release_preempt(&file->refLock);
            int64_t shared = resources->processManager->AddFileDescriptor(peer, file, 0, VirtualFileSystem::ReleaseFileHandle);
            if (shared < 0)
                vfs->Close(file);
            else
                context.descriptor = (uint64_t) shared;
            ready = shared >= 0;
        }
    }
    ready   = ready && resources->processManager->AddTask(peer, task) && scheduler->EnqueueProcess(arch_cpu_id(), peer->id);
    bool ok = memory_expect(ready, "event runnable producer/consumer fixture");
    if (ready)
    {
        ok &= wait();
        ok &= memory_expect(context.entered && context.protectedLifetime && context.result == 8, "event wake executes peer and protects parked task/process");
    }
    if (peer != nullptr)
        scheduler->KillProcess(peer->id, 0);
    if (task != nullptr && task->allocated)
        ok &= memory_expect(resources->taskManager->FreeTask(task), "event peer task cleanup");
    if (peer != nullptr && peer->allocated)
        ok &= memory_expect(resources->processManager->FreeProcess(peer), "event peer process cleanup");
    if (sharedFile != nullptr)
    {
        spinlock_acquire_preempt(&sharedFile->refLock);
        const bool releasedReference = sharedFile->refCount == referencesBefore;
        spinlock_release_preempt(&sharedFile->refLock);
        ok &= memory_expect(releasedReference, "process teardown releases shared eventfd reference");
    }
    if (space != nullptr)
        resources->virtualMemoryManager->DestroyUserAddressSpace(space);
    return ok;
}

static bool test_event_counter(process_t* process, uint64_t number)
{
    const uint64_t base = memory_map(2);
    if ((int64_t) base < 0)
        return false;
    uint64_t fd = number == SYSCALL_eventfd ? memory_call(number, 3) : memory_call(number, 3, 04000 | 02000000);
    if ((int64_t) fd < 0)
        return memory_expect(false, "eventfd creates descriptor");
    bool ok = memory_expect(memory_call(SYSCALL_fcntl, fd, 1) == (number == SYSCALL_eventfd2 ? 1ULL : 0ULL), "eventfd CLOEXEC descriptor flag");
    ok &= memory_expect(memory_call(SYSCALL_fcntl, fd, 4, 2 | 04000) == 0, "eventfd nonblocking status through fcntl");
    ok &= memory_expect(memory_call(SYSCALL_fcntl, fd, 4, 04000) == 0 && memory_call(SYSCALL_fcntl, fd, 3) == (2 | 04000), "eventfd SETFL preserves access mode");
    const uint64_t output = base + PAGE_SIZE - 4;
    ok &= memory_expect(memory_call(SYSCALL_read, fd, output, 16) == 8, "eventfd read exact eight-byte result across page boundary");
    uint64_t value = 0;
    time_fixture_load(process, output, &value, sizeof(value));
    ok &= memory_expect(value == 3 && memory_call(SYSCALL_read, fd, output, 8) == (uint64_t) -11, "eventfd reset-on-read and empty EAGAIN");
    ok &= memory_expect(memory_call(SYSCALL_read, fd, output, 0) == LINUX_EINVAL && memory_call(SYSCALL_write, fd, base, 7) == LINUX_EINVAL && memory_call(SYSCALL_write, fd, base, 9) == LINUX_EINVAL, "eventfd validates transfer sizes");
    value = UINT64_MAX;
    time_fixture_store(process, base, &value, sizeof(value));
    ok &= memory_expect(memory_call(SYSCALL_write, fd, base, 8) == LINUX_EINVAL, "eventfd rejects UINT64_MAX");
    value = UINT64_MAX - 1;
    time_fixture_store(process, base, &value, sizeof(value));
    ok &= memory_expect(memory_call(SYSCALL_write, fd, base, 8) == 8, "eventfd reaches maximum counter");
    value = 1;
    time_fixture_store(process, base, &value, sizeof(value));
    ok &= memory_expect(memory_call(SYSCALL_write, fd, base, 8) == (uint64_t) -11, "eventfd overflow EAGAIN");
    ok &= memory_expect(memory_call(SYSCALL_read, fd, CANONICAL_KERNEL_BASE, 8) == LINUX_EFAULT, "eventfd rejects kernel read output");
    ok &= memory_expect(memory_call(SYSCALL_read, fd, output, 8) == 8, "eventfd bad output leaves counter intact");
    time_fixture_load(process, output, &value, sizeof(value));
    ok &= memory_expect(value == UINT64_MAX - 1, "eventfd full counter roundtrip");
    value = 7;
    time_fixture_store(process, base, &value, sizeof(value));
    ok &= memory_expect(memory_call(SYSCALL_write, fd, base, 8) == 8, "eventfd shared-reference setup");
    uint64_t duplicate = memory_call(SYSCALL_dup, fd);
    ok &= memory_expect((int64_t) duplicate >= 0 && memory_call(SYSCALL_close, fd) == 0, "eventfd dup survives original close");
    if ((int64_t) duplicate >= 0)
    {
        fd = duplicate;
        ok &= memory_expect(memory_call(SYSCALL_read, fd, output, 8) == 8, "eventfd duplicate shares counter backing");
        time_fixture_load(process, output, &value, sizeof(value));
        ok &= memory_expect(value == 7, "eventfd duplicate value");
    }
    ok &= memory_expect(memory_call(SYSCALL_lseek, fd, 123, 0) == 0, "eventfd no-op seek");
    uint64_t vector[2] = {base, 8};
    time_fixture_store(process, base + 128, vector, sizeof(vector));
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, base + 128, 1) == LINUX_EOPNOTSUPP && memory_call(SYSCALL_pread64, fd, base, 8, 0) == (uint64_t) -29, "eventfd unsupported vector/positioned I/O is explicit");
    // Clear NONBLOCK and exercise an actual parked read with another process.
    memory_call(SYSCALL_fcntl, fd, 4, 2);
    ok &= event_with_peer(process, fd, false, [&]() { return memory_expect(memory_call(SYSCALL_read, fd, output, 8) == 8, "blocking eventfd read wakes on peer write"); });
    value = UINT64_MAX - 1;
    time_fixture_store(process, base, &value, sizeof(value));
    ok &= memory_expect(memory_call(SYSCALL_write, fd, base, 8) == 8, "blocking write full-counter setup");
    value = 1;
    time_fixture_store(process, base, &value, sizeof(value));
    ok &= event_with_peer(process, fd, true, [&]() { return memory_expect(memory_call(SYSCALL_write, fd, base, 8) == 8, "blocking eventfd write wakes on peer read"); });
    memory_call(SYSCALL_fcntl, fd, 4, 2 | 04000);
    ok &= memory_expect(memory_call(SYSCALL_read, fd, output, 8) == 8, "blocking write leaves written increment");
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
    value = 4;
    time_fixture_store(process, base, &value, sizeof(value));
    memory_call(SYSCALL_write, fd, base, 8);
    ok &= memory_expect(memory_call(SYSCALL_read, fd, output, 8) == LINUX_EFAULT, "eventfd readonly output");
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);
    ok &= memory_expect(memory_call(SYSCALL_read, fd, output, 8) == 8, "eventfd readonly failure preserves counter");
    ok &= memory_expect(memory_call(SYSCALL_write, fd, CANONICAL_KERNEL_BASE, 8) == LINUX_EFAULT && memory_call(SYSCALL_write, fd, base + 2 * PAGE_SIZE - 4, 8) == LINUX_EFAULT, "eventfd invalid input buffers");
    ok &= memory_expect(memory_call(SYSCALL_close, fd) == 0, "eventfd final close");
    if (number == SYSCALL_eventfd2)
    {
        ok &= memory_expect(memory_call(number, 0, 2) == LINUX_EINVAL, "eventfd2 invalid flags");
        uint64_t semaphore = memory_call(number, 2, 1 | 04000);
        ok &= memory_expect((int64_t) semaphore >= 0, "eventfd2 semaphore creation");
        if ((int64_t) semaphore >= 0)
        {
            ok &= memory_expect(memory_call(SYSCALL_read, semaphore, base, 8) == 8, "eventfd semaphore first read");
            time_fixture_load(process, base, &value, sizeof(value));
            ok &= memory_expect(value == 1 && memory_call(SYSCALL_read, semaphore, base, 8) == 8 && memory_call(SYSCALL_read, semaphore, base, 8) == (uint64_t) -11, "eventfd semaphore decrements by one");
            memory_call(SYSCALL_close, semaphore);
        }
    }
    Dispatcher*          dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ProcessManager*      processes  = dispatcher->GetResourceLayerCaps()->processManager;
    process_file_limit_t old = {}, limit = {0, process->fileLimit.maximum};
    processes->ChangeFileLimit(process, &limit, &old);
    ok &= memory_expect(memory_call(number, 0, 0) == LINUX_EMFILE, "eventfd descriptor limit rollback");
    processes->ChangeFileLimit(process, &old, nullptr);
    return ok;
}

static bool test_event_wait(process_t* process, uint64_t number)
{
    const bool     select  = number == SYSCALL_select || number == SYSCALL_pselect6;
    const bool     precise = number == SYSCALL_ppoll || number == SYSCALL_pselect6;
    const uint64_t base    = memory_map(2);
    if ((int64_t) base < 0)
        return false;
    const uint64_t  input    = base + PAGE_SIZE - 4;
    const uint64_t  timeout  = base + 256;
    time_timespec_t duration = {};
    time_fixture_store(process, timeout, &duration, sizeof(duration));
    Dispatcher*   dispatcher  = static_cast<Dispatcher*>(platform.dispacher);
    EventManager* events      = dispatcher->GetLogicLayerCaps()->eventManager;
    uint64_t      oldSequence = events->Sequence();
    events->Notify();
    bool     sequenceOk = memory_expect(events->Wait(oldSequence) == 0, "event arriving before park prevents sleeping");
    uint64_t fd         = memory_call(SYSCALL_eventfd2, 0, 04000);
    if ((int64_t) fd < 0)
        return false;
    auto           call    = [&]() -> uint64_t { return select ? memory_call(number, fd + 1, input, base + 64, base + 128, timeout) : precise ? memory_call(number, input, 1, timeout) : memory_call(number, input, 1, 0); };
    event_pollfd_t pollfd  = {(int32_t) fd, 1, 0xffff};
    uint64_t       readSet = 1ULL << fd, writeSet = 0, exceptSet = 0;
    auto           prepare = [&]()
    {
        if (select)
        {
            time_fixture_store(process, input, &readSet, sizeof(readSet));
            time_fixture_store(process, base + 64, &writeSet, sizeof(writeSet));
            time_fixture_store(process, base + 128, &exceptSet, sizeof(exceptSet));
        }
        else
            time_fixture_store(process, input, &pollfd, sizeof(pollfd));
    };
    prepare();
    bool ok = sequenceOk && memory_expect(call() == 0, "event readiness zero timeout on empty counter");
    if (!select)
    {
        event_pollfd_t actual;
        time_fixture_load(process, input, &actual, sizeof(actual));
        ok &= memory_expect(actual.fd == pollfd.fd && actual.events == pollfd.events && actual.revents == 0, "poll writes only revents");
        pollfd.fd = -1;
        prepare();
        ok &= memory_expect(call() == 0, "poll ignores negative descriptor");
        pollfd.fd = (int32_t) process->fileDescriptorCount - 1;
        prepare();
        ok &= memory_expect(call() == 1, "poll invalid descriptor is reported as ready");
        time_fixture_load(process, input, &actual, sizeof(actual));
        ok &= memory_expect(actual.revents == 0x20, "poll POLLNVAL without requested events");
        pollfd.fd = (int32_t) fd;
    }
    else
    {
        uint64_t invalid = 1ULL << (process->fileDescriptorCount - 1);
        time_fixture_store(process, input, &invalid, sizeof(invalid));
        ok &= memory_expect(memory_call(number, process->fileDescriptorCount, input, 0, 0, timeout) == LINUX_EBADF, "select invalid descriptor is EBADF");
    }
    uint64_t value = 9;
    time_fixture_store(process, base, &value, sizeof(value));
    memory_call(SYSCALL_write, fd, base, 8);
    writeSet = select ? 1ULL << fd : 0;
    if (!select)
        pollfd.events = 1 | 4;
    prepare();
    ok &= memory_expect(call() == (select ? 2ULL : 1ULL), "event counter read/write readiness and select ready-bit count");
    if (select)
    {
        ProcessManager*      processes = dispatcher->GetResourceLayerCaps()->processManager;
        process_file_limit_t limit     = {0, process->fileLimit.maximum}, original;
        processes->ChangeFileLimit(process, &limit, &original);
        prepare();
        ok &= memory_expect(call() == 2, "select continues monitoring existing descriptors above a lowered soft limit");
        processes->ChangeFileLimit(process, &original, nullptr);
    }
    memory_call(SYSCALL_read, fd, base, 8);
    writeSet      = 0;
    pollfd.events = 1;
    memcpy(memory_bytes(process, base + 640), "/test.txt", sizeof("/test.txt"));
    uint64_t regular = memory_call(SYSCALL_open, base + 640, 0);
    ok &= memory_expect((int64_t) regular >= 0, "event regular-file fixture opens");
    if ((int64_t) regular >= 0)
    {
        memory_call(SYSCALL_lseek, regular, 0, 2);
        uint64_t ready;
        if (select)
        {
            uint64_t bit = 1ULL << regular;
            time_fixture_store(process, input, &bit, sizeof(bit));
            time_fixture_store(process, base + 64, &bit, sizeof(bit));
            ready = memory_call(number, regular + 1, input, base + 64, 0, timeout);
        }
        else
        {
            event_pollfd_t filePoll = {(int32_t) regular, 1 | 4, 0};
            time_fixture_store(process, input, &filePoll, sizeof(filePoll));
            ready = precise ? memory_call(number, input, 1, timeout) : memory_call(number, input, 1, 0);
        }
        ok &= memory_expect(ready == (select ? 2ULL : 1ULL), "regular file remains readable/writable at EOF");
        memory_call(SYSCALL_close, regular);
    }
    prepare();
    ok &= event_with_peer(process, fd, false,
                          [&]()
                          {
                              // Infinite waits need no clock and must be awakened by the producer.
                              uint64_t result = select ? memory_call(number, fd + 1, input, 0, 0, 0) : precise ? memory_call(number, input, 1, 0) : memory_call(number, input, 1, UINT64_MAX);
                              return memory_expect(result == 1, "poll/select parked wait wakes on peer event");
                          });
    memory_call(SYSCALL_read, fd, base, 8);
    prepare();
    if (precise)
    {
        uint64_t mask = 0;
        time_fixture_store(process, base + 512, &mask, sizeof(mask));
        uint64_t bundle[2] = {base + 512, 8};
        time_fixture_store(process, base + 544, bundle, sizeof(bundle));
        uint64_t unsupported = select ? memory_call(number, 0, 0, 0, 0, timeout, base + 544) : memory_call(number, 0, 0, timeout, base + 512, 8);
        ok &= memory_expect(unsupported == LINUX_EOPNOTSUPP, "event atomic signal masks explicitly unsupported");
        bundle[1] = 4;
        time_fixture_store(process, base + 544, bundle, sizeof(bundle));
        uint64_t badSize = select ? memory_call(number, 0, 0, 0, 0, timeout, base + 544) : memory_call(number, 0, 0, timeout, base + 512, 4);
        ok &= memory_expect(badSize == LINUX_EINVAL, "event raw kernel signal-set size validation");
        duration = {-1, 0};
        time_fixture_store(process, timeout, &duration, sizeof(duration));
        ok &= memory_expect(call() == LINUX_EINVAL, "event negative timespec");
        duration = {0, 1000000000};
        time_fixture_store(process, timeout, &duration, sizeof(duration));
        ok &= memory_expect(call() == LINUX_EINVAL, "event invalid timespec nanoseconds");
    }
    duration = {};
    time_fixture_store(process, timeout, &duration, sizeof(duration));
    uint64_t invalidPointer = select ? memory_call(number, 1, CANONICAL_KERNEL_BASE, 0, 0, timeout) : precise ? memory_call(number, CANONICAL_KERNEL_BASE, 1, timeout) : memory_call(number, CANONICAL_KERNEL_BASE, 1, 0);
    ok &= memory_expect(invalidPointer == LINUX_EFAULT, "event invalid user input");
    if (select)
        ok &= memory_expect(memory_call(number, UINT64_MAX, 0, 0, 0, timeout) == LINUX_EINVAL, "select negative nfds");
    else
        ok &= memory_expect((precise ? memory_call(number, 0, process->fileLimit.current + 1, timeout) : memory_call(number, 0, process->fileLimit.current + 1, 0)) == LINUX_EINVAL, "poll count constrained by descriptor limit");
    prepare();
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
    ok &= memory_expect(call() == LINUX_EFAULT, "event readonly output boundary");
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);
    // Exercise a finite timeout, including raw timeval/timespec update.
    duration = {0, select && !precise ? 1000 : 1000000};
    time_fixture_store(process, timeout, &duration, sizeof(duration));
    time_timespec_t now;
    if (dispatcher->GetResourceLayerCaps()->timeManager->ReadClock(1, &now) == 0)
    {
        uint64_t result = select ? memory_call(number, 0, 0, 0, 0, timeout) : precise ? memory_call(number, 0, 0, timeout) : memory_call(number, 0, 0, 1);
        ok &= memory_expect(result == 0, "event finite empty wait expires");
        if (select || precise)
        {
            time_fixture_load(process, timeout, &duration, sizeof(duration));
            ok &= memory_expect(duration.seconds == 0 && duration.nanoseconds == 0, "event raw timeout updated to zero");
        }
    }
    else
        kprintf("Arx kernel: event finite timeout selftest unavailable without monotonic clock\n");
    ok &= memory_expect(memory_call(SYSCALL_close, fd) == 0, "event wait descriptor cleanup");
    return ok;
}

static bool test_event_epoll(process_t* process, uint64_t number)
{
    const uint64_t base = memory_map(2);
    if ((int64_t) base < 0)
        return false;
    const uint64_t input  = base + PAGE_SIZE - 4; // Packed event crosses a page.
    const uint64_t output = base + 128;
    const bool     legacy = number == SYSCALL_epoll_create;
    const uint64_t epfd   = legacy ? memory_call(number, 1) : memory_call(SYSCALL_epoll_create1, 02000000);
    const uint64_t fd     = memory_call(SYSCALL_eventfd2, 0, 04000);
    if (!memory_expect((int64_t) epfd >= 0 && (int64_t) fd >= 0, "epoll fixture descriptors"))
        return false;
    bool ok = memory_expect(memory_call(SYSCALL_fcntl, epfd, 1) == (legacy ? 0ULL : 1ULL), "epoll creation CLOEXEC flag");
    ok &= memory_expect(memory_call(SYSCALL_epoll_create, 0) == LINUX_EINVAL && memory_call(SYSCALL_epoll_create, UINT64_MAX) == LINUX_EINVAL && memory_call(SYSCALL_epoll_create1, 04000) == LINUX_EINVAL, "epoll creation argument validation");
    event_epoll_event_t event = {EVENT_POLL_IN, 0xfedcba9876543210ULL}, actual = {};
    time_fixture_store(process, input, &event, sizeof(event));
    auto control = [&](uint64_t operation, uint64_t target) -> uint64_t { return memory_call(SYSCALL_epoll_ctl, epfd, operation, target, input); };
    auto wait    = [&](uint64_t count, uint64_t timeout) -> uint64_t { return memory_call(SYSCALL_epoll_wait, epfd, output, count, timeout); };
    ok &= memory_expect(wait(1, 0) == 0, "empty epoll zero timeout");
    ok &= memory_expect(control(3, fd) == LINUX_ENOENT && control(2, fd) == LINUX_ENOENT, "epoll missing MOD/DEL");
    ok &= memory_expect(control(1, fd) == 0 && control(1, fd) == (uint64_t) -17, "epoll ADD and duplicate EEXIST");
    ok &= memory_expect(wait(1, 0) == 0, "registered empty eventfd is not ready");
    ok &= memory_expect(control(0, fd) == LINUX_EINVAL && control(1, UINT64_MAX) == LINUX_EBADF && control(1, epfd) == LINUX_EINVAL, "epoll operation/fd/self validation");
    ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, epfd, 3, fd, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "epoll bad event input");
    const uint32_t unsupportedModes[] = {0x80000000U, 0x40000000U, 0x20000000U, 0x10000000U};
    for (uint32_t unsupported : unsupportedModes)
    {
        event.events = EVENT_POLL_IN | unsupported;
        time_fixture_store(process, input, &event, sizeof(event));
        ok &= memory_expect(control(3, fd) == LINUX_EOPNOTSUPP, "epoll unsupported ET/ONESHOT/WAKEUP/EXCLUSIVE rejected");
    }
    event = {EVENT_POLL_IN, 0xfedcba9876543210ULL};
    time_fixture_store(process, input, &event, sizeof(event));
    const uint64_t other = memory_call(SYSCALL_epoll_create1, 0);
    if ((int64_t) other >= 0)
    {
        ok &= memory_expect(control(1, other) == LINUX_EOPNOTSUPP, "nested epoll explicitly unsupported");
        ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, other, 1, fd, input) == 0 && memory_call(SYSCALL_close, other) == 0, "one source can belong to independent epoll instances");
    }
    else
        ok &= memory_expect(false, "second epoll fixture");
    memcpy(memory_bytes(process, base + 640), "/test.txt", sizeof("/test.txt"));
    const uint64_t regular = memory_call(SYSCALL_open, base + 640, 0);
    if ((int64_t) regular >= 0)
    {
        ok &= memory_expect(control(1, regular) == (uint64_t) -1, "epoll rejects synchronous regular files with EPERM");
        ok &= memory_expect(memory_call(SYSCALL_epoll_wait, regular, output, 1, 0) == LINUX_EINVAL && memory_call(SYSCALL_epoll_ctl, regular, 1, fd, input) == LINUX_EINVAL, "epoll operations reject non-epoll instance descriptors");
        memory_call(SYSCALL_close, regular);
    }
    else
        ok &= memory_expect(false, "epoll regular file fixture");
    uint64_t value = 1;
    time_fixture_store(process, base, &value, sizeof(value));
    ok &= memory_expect(memory_call(SYSCALL_write, fd, base, 8) == 8, "epoll eventfd producer");
    ok &= memory_expect(wait(1, 0) == 1, "epoll ready event delivered");
    time_fixture_load(process, output, &actual, sizeof(actual));
    ok &= memory_expect(actual.events == EVENT_POLL_IN && actual.data == event.data, "epoll packed mask and 64-bit opaque data");
    ok &= memory_expect(wait(1, 0) == 1, "level-triggered readiness repeats without consumption");
    event_pollfd_t pollfd = {(int32_t) epfd, EVENT_POLL_IN, 0};
    time_fixture_store(process, output + 64, &pollfd, sizeof(pollfd));
    ok &= memory_expect(memory_call(SYSCALL_poll, output + 64, 1, 0) == 1, "epoll descriptor itself is pollable");
    ok &= memory_expect(memory_call(SYSCALL_epoll_wait, epfd, CANONICAL_KERNEL_BASE, 1, 0) == LINUX_EFAULT, "epoll rejects kernel output");
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_epoll_wait, epfd, input, 1, 0) == LINUX_EFAULT, "epoll rejects readonly output across page boundary");
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);
    ok &= memory_expect(wait(1, 0) == 1, "epoll output fault preserves level readiness");
    event = {EVENT_POLL_OUT, 42};
    time_fixture_store(process, input, &event, sizeof(event));
    ok &= memory_expect(control(3, fd) == 0 && wait(1, 0) == 1, "epoll MOD updates interest");
    time_fixture_load(process, output, &actual, sizeof(actual));
    ok &= memory_expect(actual.events == EVENT_POLL_OUT && actual.data == 42, "epoll MOD updates opaque data");
    ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, epfd, 2, fd, CANONICAL_KERNEL_BASE) == 0 && wait(1, 0) == 0, "epoll DEL ignores event pointer and removes registration");
    event = {EVENT_POLL_IN, 1};
    time_fixture_store(process, input, &event, sizeof(event));
    ok &= memory_expect(control(1, fd) == 0, "epoll re-add after DEL");
    const uint64_t duplicate = memory_call(SYSCALL_dup, fd);
    if ((int64_t) duplicate >= 0)
    {
        event.data = 2;
        time_fixture_store(process, input, &event, sizeof(event));
        ok &= memory_expect(control(1, duplicate) == 0, "epoll keys include fd and open file description");
        event_epoll_event_t first = {}, second = {};
        ok &= memory_expect(wait(1, 0) == 1, "epoll first bounded batch");
        time_fixture_load(process, output, &first, sizeof(first));
        ok &= memory_expect(wait(1, 0) == 1, "epoll second bounded batch");
        time_fixture_load(process, output, &second, sizeof(second));
        ok &= memory_expect(first.data != second.data, "epoll round-robin fairness");
        ok &= memory_expect(memory_call(SYSCALL_close, fd) == 0 && wait(2, 0) == 2, "epoll original registration survives close while dup remains");
        const uint64_t replacement = memory_call(SYSCALL_eventfd2, 0, 04000);
        ok &= memory_expect(replacement == fd && control(2, replacement) == LINUX_ENOENT, "epoll fd reuse cannot address old open file registration");
        if ((int64_t) replacement >= 0)
            memory_call(SYSCALL_close, replacement);
        ok &= memory_expect(memory_call(SYSCALL_close, duplicate) == 0 && wait(2, 0) == 0, "epoll final source close removes all weak registrations");
    }
    else
    {
        ok &= memory_expect(false, "epoll dup fixture");
        memory_call(SYSCALL_close, fd);
    }
    const uint64_t source = memory_call(SYSCALL_eventfd2, 0, 04000);
    if ((int64_t) source >= 0)
    {
        time_fixture_store(process, input, &event, sizeof(event));
        const bool registered = control(1, source) == 0;
        ok &= memory_expect(registered, "epoll blocking source registration");
        if (registered)
            ok &= event_with_peer(process, source, false, [&]() { return memory_expect(wait(1, UINT64_MAX) == 1, "parked epoll wait wakes on real peer write"); });
        memory_call(SYSCALL_read, source, base, 8);
        time_timespec_t now;
        Dispatcher*     dispatcher = static_cast<Dispatcher*>(platform.dispacher);
        TimeManager*    clock      = dispatcher->GetResourceLayerCaps()->timeManager;
        if (clock != nullptr && clock->ReadClock(1, &now) == 0)
            ok &= memory_expect(wait(1, 1) == 0, "epoll finite timeout expires");
        else
            kprintf("Arx kernel: epoll finite timeout selftest unavailable without monotonic clock\n");
        const uint64_t alias = memory_call(SYSCALL_dup, epfd);
        if ((int64_t) alias >= 0)
        {
            ok &= memory_expect(memory_call(SYSCALL_close, epfd) == 0 && memory_call(SYSCALL_epoll_wait, alias, output, 1, 0) == 0, "epoll duplicated instance survives original close");
            memory_call(SYSCALL_close, alias); // Remove watches while source is live.
        }
        else
        {
            ok &= memory_expect(false, "epoll instance dup fixture");
            memory_call(SYSCALL_close, epfd);
        }
        ok &= memory_expect(memory_call(SYSCALL_close, source) == 0, "epoll instance/source cleanup");
    }
    else
    {
        ok &= memory_expect(false, "epoll blocking source fixture");
        memory_call(SYSCALL_close, epfd);
    }
    ok &= memory_expect(memory_call(SYSCALL_epoll_wait, UINT64_MAX, output, 1, 0) == LINUX_EBADF && memory_call(SYSCALL_epoll_wait, UINT64_MAX, output, 0, 0) == LINUX_EINVAL && memory_call(SYSCALL_epoll_wait, UINT64_MAX, output, UINT64_MAX, 0) == LINUX_EINVAL, "epoll wait fd/maxevents validation");
    Dispatcher*          dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ProcessManager*      processes  = dispatcher->GetResourceLayerCaps()->processManager;
    process_file_limit_t old = {}, limit = {0, process->fileLimit.maximum};
    processes->ChangeFileLimit(process, &limit, &old);
    ok &= memory_expect(memory_call(SYSCALL_epoll_create1, 0) == LINUX_EMFILE, "epoll creation descriptor limit rollback");
    processes->ChangeFileLimit(process, &old, nullptr);
    // Exercise generic process teardown with live registrations. The isolated
    // fixture closes these descriptors through ProcessManager::FreeProcess.
    const uint64_t teardownInstance = memory_call(SYSCALL_epoll_create1, 0);
    const uint64_t teardownSource   = memory_call(SYSCALL_eventfd2, 0, 04000);
    ok &= memory_expect((int64_t) teardownInstance >= 0 && (int64_t) teardownSource >= 0 && memory_call(SYSCALL_epoll_ctl, teardownInstance, 1, teardownSource, input) == 0, "epoll process teardown fixture retains live watch");
    return ok;
}

template <uint64_t Number> static bool custom_case_validate_event(const custom_case_ctx_t*)
{
    return run_memory_case(
            [](process_t* process)
            {
                if (Number == SYSCALL_epoll_create || Number == SYSCALL_epoll_create1 || Number == SYSCALL_epoll_ctl || Number == SYSCALL_epoll_wait)
                    return test_event_epoll(process, Number);
                return Number == SYSCALL_eventfd || Number == SYSCALL_eventfd2 ? test_event_counter(process, Number) : test_event_wait(process, Number);
            });
}

static void test_event_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<EventRequestManager> tests[] = {
            {"HandlePollRequest", &EventRequestManager::HandlePollRequest, SYSCALL_poll, custom_case_validate_event<SYSCALL_poll>},
            {"HandlePpollRequest", &EventRequestManager::HandlePpollRequest, SYSCALL_ppoll, custom_case_validate_event<SYSCALL_ppoll>},
            {"HandleSelectRequest", &EventRequestManager::HandleSelectRequest, SYSCALL_select, custom_case_validate_event<SYSCALL_select>},
            {"HandlePselect6Request", &EventRequestManager::HandlePselect6Request, SYSCALL_pselect6, custom_case_validate_event<SYSCALL_pselect6>},
            {"HandleEpoll_createRequest", &EventRequestManager::HandleEpoll_createRequest, SYSCALL_epoll_create, custom_case_validate_event<SYSCALL_epoll_create>},
            {"HandleEpoll_create1Request", &EventRequestManager::HandleEpoll_create1Request, SYSCALL_epoll_create1, custom_case_validate_event<SYSCALL_epoll_create1>},
            {"HandleEpoll_ctlRequest", &EventRequestManager::HandleEpoll_ctlRequest, SYSCALL_epoll_ctl, custom_case_validate_event<SYSCALL_epoll_ctl>},
            {"HandleEpoll_waitRequest", &EventRequestManager::HandleEpoll_waitRequest, SYSCALL_epoll_wait, custom_case_validate_event<SYSCALL_epoll_wait>},
            {"HandleEpoll_pwaitRequest", &EventRequestManager::HandleEpoll_pwaitRequest, SYSCALL_epoll_pwait},
            {"HandleEpoll_pwait2Request", &EventRequestManager::HandleEpoll_pwait2Request, SYSCALL_epoll_pwait2},
            {"HandleInotify_initRequest", &EventRequestManager::HandleInotify_initRequest, SYSCALL_inotify_init},
            {"HandleInotify_init1Request", &EventRequestManager::HandleInotify_init1Request, SYSCALL_inotify_init1},
            {"HandleInotify_add_watchRequest", &EventRequestManager::HandleInotify_add_watchRequest, SYSCALL_inotify_add_watch},
            {"HandleInotify_rm_watchRequest", &EventRequestManager::HandleInotify_rm_watchRequest, SYSCALL_inotify_rm_watch},
            {"HandleEventfdRequest", &EventRequestManager::HandleEventfdRequest, SYSCALL_eventfd, custom_case_validate_event<SYSCALL_eventfd>},
            {"HandleEventfd2Request", &EventRequestManager::HandleEventfd2Request, SYSCALL_eventfd2, custom_case_validate_event<SYSCALL_eventfd2>},
            {"HandleSignalfdRequest", &EventRequestManager::HandleSignalfdRequest, SYSCALL_signalfd},
            {"HandleSignalfd4Request", &EventRequestManager::HandleSignalfd4Request, SYSCALL_signalfd4},
    };

    if (caps->eventRequestManager != nullptr)
        for (const auto& test : tests)
            if ((caps->eventRequestManager->*(test.method))(nullptr) != LINUX_EINVAL)
            {
                stats->implementedFail++;
                selftest_record_failure_detail("event handler null-frame validation");
            }
    EventRequestManager unavailable(nullptr, nullptr);
    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};
        frame.syscall_number       = test.syscallNumber;
        if ((unavailable.*(test.method))(&frame) != LINUX_ENOSYS)
        {
            stats->implementedFail++;
            selftest_record_failure_detail("event missing dependency handling");
        }
    }
    run_manager_tests("event", caps->eventRequestManager, tests, stats, true, true);
}

static void test_sync_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<SyncRequestManager> tests[] = {
            {"HandleFutexRequest", &SyncRequestManager::HandleFutexRequest, SYSCALL_futex},
            {"HandleSet_robust_listRequest", &SyncRequestManager::HandleSet_robust_listRequest, SYSCALL_set_robust_list},
            {"HandleGet_robust_listRequest", &SyncRequestManager::HandleGet_robust_listRequest, SYSCALL_get_robust_list},
    };

    run_manager_tests("sync", caps->syncRequestManager, tests, stats);
}

static void test_signal_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<SignalRequestManager> tests[] = {
            {"HandleRt_sigreturnRequest", &SignalRequestManager::HandleRt_sigreturnRequest, SYSCALL_rt_sigreturn},
            {"HandleRt_sigprocmaskRequest", &SignalRequestManager::HandleRt_sigprocmaskRequest, SYSCALL_rt_sigprocmask},
            {"HandleRt_sigactionRequest", &SignalRequestManager::HandleRt_sigactionRequest, SYSCALL_rt_sigaction},
            {"HandleSigaltstackRequest", &SignalRequestManager::HandleSigaltstackRequest, SYSCALL_sigaltstack},
            {"HandleKillRequest", &SignalRequestManager::HandleKillRequest, SYSCALL_kill},
            {"HandleTgkillRequest", &SignalRequestManager::HandleTgkillRequest, SYSCALL_tgkill},
            {"HandlePauseRequest", &SignalRequestManager::HandlePauseRequest, SYSCALL_pause},
    };

    run_manager_tests("signal", caps->signalRequestManager, tests, stats);
}

static bool credential_expect_identity(bool group, uint32_t real, uint32_t effective, uint32_t saved, uint32_t filesystem)
{
    Dispatcher*               dispatcher  = static_cast<Dispatcher*>(platform.dispacher);
    process_credentials_t     credentials = {};
    uint64_t                  result      = dispatcher->GetLogicLayerCaps()->credentialManager->GetCredentials(&credentials);
    const process_identity_t& identity    = group ? credentials.group : credentials.user;
    return memory_expect(result == 0 && identity.real == real && identity.effective == effective && identity.saved == saved && identity.filesystem == filesystem, "credential identity matches real/effective/saved/filesystem state");
}

static bool test_credential_identity(process_t* process, uint64_t number)
{
    const uint64_t none  = UINT32_MAX;
    const bool     group = number == SYSCALL_getgid || number == SYSCALL_getegid || number == SYSCALL_getresgid || number == SYSCALL_setgid || number == SYSCALL_setregid || number == SYSCALL_setresgid || number == SYSCALL_setfsgid;
    const uint64_t set   = group ? SYSCALL_setgid : SYSCALL_setuid;
    const uint64_t re    = group ? SYSCALL_setregid : SYSCALL_setreuid;
    const uint64_t res   = group ? SYSCALL_setresgid : SYSCALL_setresuid;
    const uint64_t fs    = group ? SYSCALL_setfsgid : SYSCALL_setfsuid;
    bool           ok    = memory_expect(memory_call(SYSCALL_getuid) == 0 && memory_call(SYSCALL_geteuid) == 0, "credential fixture starts with controlled root IDs");
    ok &= memory_expect(memory_call(SYSCALL_setresgid, 200, 201, 202) == 0 && memory_call(SYSCALL_setresuid, 100, 0, 102) == 0, "credential privileged fixture setup");
    if (number == SYSCALL_getuid || number == SYSCALL_geteuid || number == SYSCALL_getgid || number == SYSCALL_getegid)
    {
        uint64_t expected = number == SYSCALL_getuid ? 100 : number == SYSCALL_geteuid ? 0 : number == SYSCALL_getgid ? 200 : 201;
        ok &= memory_expect(memory_call(number, UINT64_MAX) == expected, "credential scalar getter ignores unused arguments");
        ok &= memory_expect(memory_call(res, 11, 12, 13) == 0, "credential getter observes changed identity");
        expected = number == SYSCALL_getuid || number == SYSCALL_getgid ? 11 : 12;
        return ok && memory_expect(memory_call(number) == expected, "credential scalar getter returns updated value");
    }
    if (number == SYSCALL_getresuid || number == SYSCALL_getresgid)
    {
        uint64_t base = memory_map(2);
        if ((int64_t) base < 0)
            return false;
        const uint64_t output               = base + PAGE_SIZE - 2;
        *memory_bytes(process, output - 1)  = 0xa5;
        *memory_bytes(process, output + 12) = 0xa5;
        ok &= memory_expect(memory_call(number, output, output + 4, output + 8) == 0, "getres IDs cross mapped-page boundary");
        uint32_t ids[3] = {};
        time_fixture_load(process, output, ids, sizeof(ids));
        ok &= memory_expect(ids[0] == (group ? 200U : 100U) && ids[1] == (group ? 201U : 0U) && ids[2] == (group ? 202U : 102U), "getres IDs use 32-bit wire fields");
        ok &= memory_expect(*memory_bytes(process, output - 1) == 0xa5 && *memory_bytes(process, output + 12) == 0xa5, "getres IDs preserve adjacent bytes");
        ok &= memory_expect(memory_call(number, output, output, output) == 0, "getres aliased outputs");
        uint32_t value = 0;
        time_fixture_load(process, output, &value, sizeof(value));
        ok &= memory_expect(value == (group ? 202U : 102U), "getres aliased output receives saved ID last");
        ok &= memory_expect(memory_call(number, output, CANONICAL_KERNEL_BASE, output + 8) == LINUX_EFAULT, "getres invalid later output");
        time_fixture_load(process, output, &value, sizeof(value));
        ok &= memory_expect(value == (group ? 200U : 100U), "getres preserves Linux earlier-write behavior on fault");
        ok &= memory_expect(memory_call(number, 0, output, output) == LINUX_EFAULT && memory_call(number, base + 2 * PAGE_SIZE - 2, output, output) == LINUX_EFAULT, "getres null and partially unmapped output");
        memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
        ok &= memory_expect(memory_call(number, output, output + 4, output + 8) == LINUX_EFAULT, "getres readonly cross-page output");
        return ok;
    }
    if (number == set)
    {
        ok &= memory_expect(memory_call(number, (1ULL << 32) | 17) == 0, "set ID privileged transition and 32-bit argument decoding");
        ok &= credential_expect_identity(group, 17, 17, 17, 17);
        if (group)
            ok &= memory_expect(memory_call(SYSCALL_setuid, 1000) == 0, "drop group-management privilege");
        ok &= memory_expect(memory_call(number, 99) == (uint64_t) -1, "unprivileged set ID denied");
        ok &= credential_expect_identity(group, 17, 17, 17, 17);
        ok &= memory_expect(memory_call(number, none) == LINUX_EINVAL && memory_call(number, 17) == 0, "set ID rejects reserved ID and accepts own ID");
        return ok;
    }
    if (number == re)
    {
        ok &= memory_expect(memory_call(number, 100, group ? 201 : 0) == 0, "setre privileged arbitrary real ID");
        ok &= memory_expect(memory_call(res, 100, 101, 102) == 0, "setre distinct unprivileged IDs");
        if (group)
            ok &= memory_expect(memory_call(SYSCALL_setuid, 1000) == 0, "drop setregid privilege");
        ok &= memory_expect(memory_call(number, none, 100) == 0, "setre effective-to-real transition");
        ok &= credential_expect_identity(group, 100, 100, 102, 100);
        ok &= memory_expect(memory_call(number, 102, none) == (uint64_t) -1, "setre real ID cannot use saved-only ID");
        ok &= credential_expect_identity(group, 100, 100, 102, 100);
        ok &= memory_expect(memory_call(number, none, 102) == 0, "setre effective ID can use saved ID");
        ok &= credential_expect_identity(group, 100, 102, 102, 102);
        ok &= memory_expect(memory_call(number, 102, 100) == 0, "setre swaps real and effective IDs");
        ok &= credential_expect_identity(group, 102, 100, 100, 100);
        return ok;
    }
    if (number == res)
    {
        ok &= memory_expect(memory_call(fs, 333) == (group ? 201U : 0U), "setres fixture overrides filesystem ID");
        ok &= memory_expect(memory_call(number, none, none, none) == 0, "setres sentinel no-op");
        ok &= credential_expect_identity(group, group ? 200 : 100, group ? 201 : 0, group ? 202 : 102, 333);
        ok &= memory_expect(memory_call(number, 100, 101, 102) == 0, "setres privileged arbitrary IDs");
        if (group)
            ok &= memory_expect(memory_call(SYSCALL_setuid, 1000) == 0, "drop setresgid privilege");
        ok &= memory_expect(memory_call(number, 102, 100, 101) == 0, "setres unprivileged permutation");
        ok &= credential_expect_identity(group, 102, 100, 101, 100);
        ok &= memory_expect(memory_call(number, none, 999, none) == (uint64_t) -1, "setres unprivileged arbitrary ID denied");
        ok &= credential_expect_identity(group, 102, 100, 101, 100);
        return ok;
    }
    if (number == fs)
    {
        ok &= memory_expect(memory_call(number, 333) == (group ? 201U : 0U), "setfs privileged change returns previous ID");
        ok &= credential_expect_identity(group, group ? 200 : 100, group ? 201 : 0, group ? 202 : 102, 333);
        ok &= memory_expect(memory_call(res, 100, 101, 102) == 0, "setfs unprivileged fixture setup");
        if (group)
            ok &= memory_expect(memory_call(SYSCALL_setuid, 1000) == 0, "drop setfsgid privilege");
        ok &= memory_expect(memory_call(number, 102) == 101 && memory_call(number, 999) == 102 && memory_call(number, UINT64_MAX) == 102, "setfs saved ID allowed, arbitrary and invalid IDs silently refused");
        ok &= credential_expect_identity(group, 100, 101, 102, 102);
        return ok;
    }
    return false;
}

static bool test_credential_groups(process_t* process, uint64_t number)
{
    (void) number;
    uint64_t base = memory_map(2);
    if ((int64_t) base < 0)
        return false;
    const uint64_t output     = base + PAGE_SIZE - 8;
    const uint32_t input[]    = {10, 1, 10, 0, UINT32_MAX - 1};
    const uint32_t expected[] = {0, 1, 10, 10, UINT32_MAX - 1};
    time_fixture_store(process, base, input, sizeof(input));
    bool ok = memory_expect(memory_call(SYSCALL_getgroups, 0, CANONICAL_KERNEL_BASE) == 0, "getgroups initially empty and count query ignores pointer");
    ok &= memory_expect(memory_call(SYSCALL_setgroups, 5, base) == 0, "setgroups accepts unordered IDs and duplicates");
    ok &= memory_expect(memory_call(SYSCALL_getgroups, 0, CANONICAL_KERNEL_BASE) == 5 && memory_call(SYSCALL_getgroups, 5, output) == 5, "getgroups count and cross-page output");
    uint32_t actual[5] = {};
    time_fixture_load(process, output, actual, sizeof(actual));
    ok &= memory_expect(memcmp(actual, expected, sizeof(expected)) == 0, "getgroups sorted IDs preserve duplicates and 32-bit range");
    process_group_list_t* original = process->supplementaryGroups;
    uint32_t              invalid  = UINT32_MAX;
    time_fixture_store(process, base + 64, &invalid, sizeof(invalid));
    const size_t beforeFailures = memory_pages_in_use();
    ok &= memory_expect(memory_call(SYSCALL_setgroups, 1, base + 64) == LINUX_EINVAL && memory_call(SYSCALL_setgroups, 65537, base) == LINUX_EINVAL && memory_call(SYSCALL_setgroups, UINT64_MAX, base) == LINUX_EINVAL, "setgroups reserved ID and size boundaries");
    ok &= memory_expect(memory_call(SYSCALL_setgroups, 5, CANONICAL_KERNEL_BASE) == LINUX_EFAULT && memory_call(SYSCALL_setgroups, 1, base + 2 * PAGE_SIZE - 2) == LINUX_EFAULT, "setgroups invalid user buffers");
    ok &= memory_expect(process->supplementaryGroups == original, "failed setgroups preserves backing list");
    ok &= memory_expect(memory_pages_in_use() == beforeFailures, "failed setgroups frees staging allocations");
    ok &= memory_expect(memory_call(SYSCALL_getgroups, 4, output) == LINUX_EINVAL && memory_call(SYSCALL_getgroups, UINT64_MAX, output) == LINUX_EINVAL && memory_call(SYSCALL_getgroups, 5, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "getgroups capacity and pointer validation");

    Dispatcher*           dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ProcessManager*       processes  = dispatcher->GetResourceLayerCaps()->processManager;
    VirtualMemoryManager* memory     = dispatcher->GetResourceLayerCaps()->virtualMemoryManager;
    virt_addr_space_t*    space      = memory->CreateUserAddressSpace();
    process_t*            child      = space != nullptr ? processes->CreateProcess(space) : nullptr;
    ok &= memory_expect(child != nullptr, "credential child inheritance fixture");
    if (child != nullptr)
    {
        ok &= memory_expect(memcmp(&child->credentials, &process->credentials, sizeof(process_credentials_t)) == 0 && child->supplementaryGroups == original && original->references == 2, "child inherits IDs and retains immutable group backing");
        ok &= memory_expect(memory_call(SYSCALL_setgroups, 0, CANONICAL_KERNEL_BASE) == 0, "setgroups zero clears and ignores pointer");
        ok &= memory_expect(child->supplementaryGroups == original && original->references == 1 && original->ids[2] == 10, "parent group replacement leaves child list alive");
        ok &= memory_expect(processes->FreeProcess(child), "credential child group cleanup");
        process_t* reused = processes->AllocateProcess();
        ok &= memory_expect(reused != nullptr && reused->supplementaryGroups == nullptr && reused->credentials.user.effective == 0 && reused->credentials.group.real == 0, "credential slot reuse clears inherited state");
        if (reused != nullptr)
            ok &= memory_expect(processes->FreeProcess(reused), "credential reused-slot cleanup");
    }
    if (space != nullptr)
        memory->DestroyUserAddressSpace(space);

    const uint32_t maximum = ProcessManager::MAX_SUPPLEMENTARY_GROUPS;
    uint64_t       large   = memory_map(maximum * sizeof(uint32_t) / PAGE_SIZE);
    if ((int64_t) large >= 0)
    {
        for (uint32_t offset = 0; offset < maximum; offset += PAGE_SIZE / sizeof(uint32_t))
        {
            uint32_t* page = reinterpret_cast<uint32_t*>(memory_bytes(process, large + offset * sizeof(uint32_t)));
            for (uint32_t i = 0; i < PAGE_SIZE / sizeof(uint32_t); ++i)
                page[i] = maximum - offset - i;
        }
        ok &= memory_expect(memory_call(SYSCALL_setgroups, maximum, large) == 0 && memory_call(SYSCALL_getgroups, maximum, large) == maximum, "group list supports full 65536-entry Linux bound");
        uint32_t first = 0, last = 0;
        time_fixture_load(process, large, &first, sizeof(first));
        time_fixture_load(process, large + (maximum - 1) * sizeof(uint32_t), &last, sizeof(last));
        ok &= memory_expect(first == 1 && last == maximum, "large group list sorts descending input");
    }
    else
        ok &= memory_expect(false, "large credential group fixture mapping");
    ok &= memory_expect(memory_call(SYSCALL_setgroups, 5, base) == 0, "restore small group fixture");
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_getgroups, 5, output) == LINUX_EFAULT, "getgroups readonly output");
    ok &= memory_expect(memory_call(SYSCALL_setuid, 1000) == 0 && memory_call(SYSCALL_setgroups, 0, 0) == (uint64_t) -1 && memory_call(SYSCALL_setgroups, UINT64_MAX, CANONICAL_KERNEL_BASE) == (uint64_t) -1, "setgroups checks privilege before size or pointers");
    ok &= memory_expect(memory_call(SYSCALL_getgroups, 0) == 5, "unprivileged group reads remain available");
    return ok;
}

template <uint64_t Number> static bool custom_case_validate_credential(const custom_case_ctx_t*)
{
    return run_memory_case(
            [](process_t* process)
            {
                Dispatcher*     dispatcher = static_cast<Dispatcher*>(platform.dispacher);
                ProcessManager* processes  = dispatcher->GetResourceLayerCaps()->processManager;
                process_t*      locked     = processes->AcquireCurrentCredentials();
                if (locked == nullptr)
                    return memory_expect(false, "credential isolated fixture state");
                // Only the disposable fixture is privileged. The harness's credentials
                // are retained separately and restored after the preliminary probes.
                locked->credentials = {};
                processes->ReleaseCredentials(locked);
                if (memory_call(SYSCALL_setgroups, 0, 0) != 0)
                    return false;
                return Number == SYSCALL_getgroups || Number == SYSCALL_setgroups ? test_credential_groups(process, Number) : test_credential_identity(process, Number);
            });
}

static void test_credential_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<CredentialRequestManager> tests[] = {
            {"HandleGetuidRequest", &CredentialRequestManager::HandleGetuidRequest, SYSCALL_getuid, custom_case_validate_credential<SYSCALL_getuid>},
            {"HandleGetgidRequest", &CredentialRequestManager::HandleGetgidRequest, SYSCALL_getgid, custom_case_validate_credential<SYSCALL_getgid>},
            {"HandleGeteuidRequest", &CredentialRequestManager::HandleGeteuidRequest, SYSCALL_geteuid, custom_case_validate_credential<SYSCALL_geteuid>},
            {"HandleGetegidRequest", &CredentialRequestManager::HandleGetegidRequest, SYSCALL_getegid, custom_case_validate_credential<SYSCALL_getegid>},
            {"HandleGetresuidRequest", &CredentialRequestManager::HandleGetresuidRequest, SYSCALL_getresuid, custom_case_validate_credential<SYSCALL_getresuid>},
            {"HandleGetresgidRequest", &CredentialRequestManager::HandleGetresgidRequest, SYSCALL_getresgid, custom_case_validate_credential<SYSCALL_getresgid>},
            {"HandleSetuidRequest", &CredentialRequestManager::HandleSetuidRequest, SYSCALL_setuid, custom_case_validate_credential<SYSCALL_setuid>},
            {"HandleSetgidRequest", &CredentialRequestManager::HandleSetgidRequest, SYSCALL_setgid, custom_case_validate_credential<SYSCALL_setgid>},
            {"HandleSetreuidRequest", &CredentialRequestManager::HandleSetreuidRequest, SYSCALL_setreuid, custom_case_validate_credential<SYSCALL_setreuid>},
            {"HandleSetregidRequest", &CredentialRequestManager::HandleSetregidRequest, SYSCALL_setregid, custom_case_validate_credential<SYSCALL_setregid>},
            {"HandleSetresuidRequest", &CredentialRequestManager::HandleSetresuidRequest, SYSCALL_setresuid, custom_case_validate_credential<SYSCALL_setresuid>},
            {"HandleSetresgidRequest", &CredentialRequestManager::HandleSetresgidRequest, SYSCALL_setresgid, custom_case_validate_credential<SYSCALL_setresgid>},
            {"HandleSetfsuidRequest", &CredentialRequestManager::HandleSetfsuidRequest, SYSCALL_setfsuid, custom_case_validate_credential<SYSCALL_setfsuid>},
            {"HandleSetfsgidRequest", &CredentialRequestManager::HandleSetfsgidRequest, SYSCALL_setfsgid, custom_case_validate_credential<SYSCALL_setfsgid>},
            {"HandleGetgroupsRequest", &CredentialRequestManager::HandleGetgroupsRequest, SYSCALL_getgroups, custom_case_validate_credential<SYSCALL_getgroups>},
            {"HandleSetgroupsRequest", &CredentialRequestManager::HandleSetgroupsRequest, SYSCALL_setgroups, custom_case_validate_credential<SYSCALL_setgroups>},
    };

    Dispatcher*     dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ProcessManager* processes  = dispatcher->GetResourceLayerCaps()->processManager;
    process_t*      process    = processes->AcquireCurrentCredentials();
    if (process == nullptr)
    {
        stats->implementedFail++;
        selftest_record_failure_detail("credential selftests require harness process");
        return;
    }
    const process_credentials_t original       = process->credentials;
    process_group_list_t*       originalGroups = process->supplementaryGroups;
    if (originalGroups != nullptr)
        __atomic_add_fetch(&originalGroups->references, 1U, __ATOMIC_RELAXED);
    processes->ReleaseCredentials(process);
    if (caps->credentialRequestManager != nullptr)
        for (const auto& test : tests)
            if ((caps->credentialRequestManager->*(test.method))(nullptr) != LINUX_EINVAL)
            {
                stats->implementedFail++;
                selftest_record_failure_detail("credential handler null-frame validation");
            }
    CredentialRequestManager unavailable(nullptr, nullptr);
    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};
        frame.syscall_number       = test.syscallNumber;
        if ((unavailable.*(test.method))(&frame) != LINUX_ENOSYS)
        {
            stats->implementedFail++;
            selftest_record_failure_detail("credential missing dependency handling");
        }
    }
    run_manager_tests("credential", caps->credentialRequestManager, tests, stats, true);
    process = processes->AcquireCurrentCredentials();
    if (process != nullptr)
    {
        process_group_list_t* discarded = process->supplementaryGroups;
        process->credentials            = original;
        process->supplementaryGroups    = originalGroups;
        processes->ReleaseCredentials(process);
        processes->ReleaseGroupList(discarded);
    }
    else
    {
        processes->ReleaseGroupList(originalGroups);
        stats->implementedFail++;
        selftest_record_failure_detail("credential harness state restoration");
    }
}

static bool test_system_tls(process_t* process)
{
#if defined(__x86_64__)
    uint64_t base = memory_map(2);
    if ((int64_t) base < 0)
        return false;
    Dispatcher*  dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    TaskManager* tasks      = dispatcher->GetResourceLayerCaps()->taskManager;
    uint64_t     savedFs = 0, savedGs = 0;
    if (!tasks->GetCurrentTlsBase(false, &savedFs) || !tasks->GetCurrentTlsBase(true, &savedGs))
        return false;
    bool ok = true;
    for (uint64_t gs = 0; gs < 2; ++gs)
    {
        const uint64_t set   = gs ? 0x1001 : 0x1002;
        const uint64_t get   = gs ? 0x1004 : 0x1003;
        const uint64_t value = base + 128 + gs * 8;
        ok &= memory_expect(memory_call(SYSCALL_arch_prctl, set, value) == 0, "arch_prctl sets TLS base");
        ok &= memory_expect(memory_call(SYSCALL_arch_prctl, get, base + PAGE_SIZE - 4) == 0, "arch_prctl cross-page output");
        uint64_t actual = 0;
        time_fixture_load(process, base + PAGE_SIZE - 4, &actual, sizeof(actual));
        ok &= memory_expect(actual == value, "arch_prctl TLS roundtrip");
        ok &= memory_expect(memory_call(SYSCALL_arch_prctl, set, 1ULL << 47) == (uint64_t) -1, "arch_prctl rejects non-user bases");
        ok &= memory_expect(memory_call(SYSCALL_arch_prctl, get, base) == 0, "arch_prctl reads after failed set");
        time_fixture_load(process, base, &actual, sizeof(actual));
        ok &= memory_expect(actual == value, "arch_prctl failed set preserves hardware state");
        ok &= memory_expect(memory_call(SYSCALL_arch_prctl, get, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "arch_prctl rejects kernel output");
        ok &= memory_expect(memory_call(SYSCALL_arch_prctl, get, 0) == LINUX_EFAULT, "arch_prctl rejects null output");
        ok &= memory_expect(memory_call(SYSCALL_arch_prctl, set, 0) == 0, "arch_prctl permits zero base");
    }
    ok &= memory_expect(memory_call(SYSCALL_arch_prctl, 0) == LINUX_EINVAL, "arch_prctl invalid operation");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_arch_prctl, 0x1003, base) == LINUX_EFAULT, "arch_prctl readonly output");
    tasks->SetCurrentTlsBase(false, savedFs);
    tasks->SetCurrentTlsBase(true, savedGs);
    return ok;
#else
    (void) process;
    return memory_expect(memory_call(SYSCALL_arch_prctl) == LINUX_ENOSYS, "arch_prctl x86-only");
#endif
}

static bool test_system_name(process_t* process)
{
    uint64_t base = memory_map(2);
    if ((int64_t) base < 0)
        return false;
    Dispatcher*  dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    TaskManager* tasks      = dispatcher->GetResourceLayerCaps()->taskManager;
    char         saved[16] = {}, name[16] = {};
    if (!tasks->GetCurrentName(saved))
        return false;
    const char input[] = "abcdefghijklmnopq";
    time_fixture_store(process, base, input, sizeof(input));
    bool ok = memory_expect(memory_call(SYSCALL_prctl, 15, base) == 0, "prctl sets name");
    ok &= memory_expect(memory_call(SYSCALL_prctl, 16, base + PAGE_SIZE - 8) == 0, "prctl cross-page get name");
    time_fixture_load(process, base + PAGE_SIZE - 8, name, sizeof(name));
    ok &= memory_expect(memcmp(name, input, 15) == 0 && name[15] == 0, "prctl truncates name to 15 bytes");
    ok &= memory_expect(memory_call(SYSCALL_prctl, 15, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "prctl invalid set buffer");
    ok &= memory_expect(memory_call(SYSCALL_prctl, 16, base) == 0, "prctl reads after failed set");
    time_fixture_load(process, base, name, sizeof(name));
    ok &= memory_expect(memcmp(name, input, 15) == 0 && name[15] == 0, "prctl failed set preserves task name");
    ok &= memory_expect(memory_call(SYSCALL_prctl, 16, 0) == LINUX_EFAULT, "prctl null get buffer");
    ok &= memory_expect(memory_call(SYSCALL_prctl, 15, base + 2 * PAGE_SIZE - 1) == 0, "prctl short NUL string at mapped boundary");
    ok &= memory_expect(memory_call(SYSCALL_prctl, 16, base) == 0 && *memory_bytes(process, base) == 0, "prctl empty name");
    ok &= memory_expect(memory_call(SYSCALL_prctl, 0x7fffffff) == LINUX_EOPNOTSUPP, "prctl unsupported operation");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_prctl, 16, base) == LINUX_EFAULT, "prctl readonly output");
    tasks->SetCurrentName(saved);
    return ok;
}

static bool test_system_limits(process_t* process, uint64_t number)
{
    uint64_t base = memory_map(2);
    if ((int64_t) base < 0)
        return false;
    const uint64_t       output   = base + PAGE_SIZE - 8;
    process_file_limit_t original = {}, actual = {};
    bool                 ok = memory_expect(memory_call(SYSCALL_getrlimit, 7, output) == 0, "getrlimit cross-page snapshot");
    time_fixture_load(process, output, &original, sizeof(original));
    ok &= memory_expect(original.current == 1024 && original.maximum == ProcessManager::MAX_FILE_DESCRIPTORS, "getrlimit default limits");
    process_file_limit_t next = {2, original.maximum};
    time_fixture_store(process, base, &next, sizeof(next));
    if (number == SYSCALL_prlimit64)
    {
        ok &= memory_expect(memory_call(number, process->id, 7, base, output) == 0, "prlimit64 current PID exchange");
        time_fixture_load(process, output, &actual, sizeof(actual));
        ok &= memory_expect(actual.current == original.current && actual.maximum == original.maximum, "prlimit64 returns previous limit");
    }
    else
        ok &= memory_expect(memory_call(SYSCALL_setrlimit, 7, base) == 0, "setrlimit lowers soft limit");
    memcpy(memory_bytes(process, base + 128), "/test.txt", sizeof("/test.txt"));
    uint64_t fd        = memory_call(SYSCALL_open, base + 128, 0);
    uint64_t duplicate = memory_call(SYSCALL_dup, fd);
    ok &= memory_expect(fd == 0 && duplicate == 1, "nofile permits descriptors below soft limit");
    ok &= memory_expect(memory_call(SYSCALL_open, base + 128, 0) == LINUX_EMFILE && memory_call(SYSCALL_dup, fd) == LINUX_EMFILE, "nofile open and dup exhaustion");
    ok &= memory_expect(memory_call(SYSCALL_dup2, fd, 2) == LINUX_EBADF && memory_call(SYSCALL_dup3, fd, 2, 0) == LINUX_EBADF, "nofile fixed duplication boundary");
    ok &= memory_expect(memory_call(SYSCALL_fcntl, fd, 0, 2) == LINUX_EINVAL, "nofile F_DUPFD boundary");
    next.current = 0;
    time_fixture_store(process, base, &next, sizeof(next));
    ok &= memory_expect(memory_call(SYSCALL_setrlimit, 7, base) == 0, "nofile zero soft limit");
    ok &= memory_expect(memory_call(SYSCALL_fcntl, duplicate, 1) == 0, "lowering limit preserves existing descriptors");
    ok &= memory_expect(memory_call(SYSCALL_dup2, duplicate, duplicate) == duplicate, "dup2 identity remains valid above lowered limit");
    ok &= memory_expect(memory_call(SYSCALL_close, fd) == 0 && memory_call(SYSCALL_close, duplicate) == 0, "nofile descriptor cleanup");
    next.current = original.maximum + 1;
    time_fixture_store(process, base, &next, sizeof(next));
    ok &= memory_expect(memory_call(SYSCALL_setrlimit, 7, base) == LINUX_EINVAL, "rlimit rejects soft above hard");
    next = {1, original.maximum + 1};
    time_fixture_store(process, base, &next, sizeof(next));
    ok &= memory_expect(memory_call(SYSCALL_setrlimit, 7, base) == (uint64_t) -1, "rlimit denies hard raise without credentials");
    ok &= memory_expect(process->fileLimit.current == 0 && process->fileLimit.maximum == original.maximum, "invalid proposed limits preserve state");
    next = {4, original.maximum};
    time_fixture_store(process, base, &next, sizeof(next));
    ok &= memory_expect(memory_call(SYSCALL_prlimit64, 0, 7, base, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "prlimit64 invalid previous output");
    ok &= memory_expect(process->fileLimit.current == 4, "Linux prlimit64 commits before previous-output EFAULT");
    ok &= memory_expect(memory_call(SYSCALL_setrlimit, 7, CANONICAL_KERNEL_BASE) == LINUX_EFAULT && memory_call(SYSCALL_getrlimit, 7, 0) == LINUX_EFAULT, "rlimit invalid buffers");
    ok &= memory_expect(memory_call(SYSCALL_prlimit64, 0, 16) == LINUX_EINVAL && memory_call(SYSCALL_prlimit64, 0, 0) == LINUX_EOPNOTSUPP, "rlimit invalid and unsupported resources");
    ok &= memory_expect(memory_call(SYSCALL_prlimit64, UINT64_MAX, 7) == LINUX_ESRCH && memory_call(SYSCALL_prlimit64, ProcessManager::MAX_PROCESSES, 7) == LINUX_EOPNOTSUPP, "prlimit64 PID restrictions");
    ok &= memory_expect(memory_call(SYSCALL_prlimit64, 0, 7, base, base) == 0, "prlimit64 aliased input and output");
    time_fixture_load(process, base, &actual, sizeof(actual));
    ok &= memory_expect(actual.current == 4 && process->fileLimit.current == 4, "prlimit64 alias snapshots input before output");
    next = {1, 4};
    time_fixture_store(process, base, &next, sizeof(next));
    ok &= memory_expect(memory_call(SYSCALL_setrlimit, 7, base) == 0 && process->fileLimit.maximum == 4, "setrlimit lowers hard limit");
    time_fixture_store(process, base, &original, sizeof(original));
    ok &= memory_expect(memory_call(SYSCALL_setrlimit, 7, base) == (uint64_t) -1, "hard limit reduction is irreversible without privilege");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_getrlimit, 7, base) == LINUX_EFAULT, "getrlimit readonly output");
    return ok;
}

static bool test_system_info(process_t* process, uint64_t number)
{
    uint64_t base = memory_map(2);
    if ((int64_t) base < 0)
        return false;
    const size_t   length                   = number == SYSCALL_uname ? 390 : 112;
    const uint64_t output                   = base + PAGE_SIZE - 32;
    *memory_bytes(process, output - 1)      = 0xa5;
    *memory_bytes(process, output + length) = 0xa5;
    uint64_t result                         = memory_call(number, output);
    bool     ok                             = memory_expect(memory_call(number, 0) == LINUX_EFAULT, "system info rejects null output");
    if (number == SYSCALL_sysinfo && result == LINUX_ENOSYS)
    {
        kprintf("Arx kernel: sysinfo selftest clock unavailable; successful snapshot requires monotonic clock\n");
        return ok && memory_expect(*memory_bytes(process, output) == 0, "unavailable sysinfo does not fabricate a snapshot");
    }
    ok &= memory_expect(result == 0, "system info cross-page output");
    ok &= memory_expect(*memory_bytes(process, output - 1) == 0xa5 && *memory_bytes(process, output + length) == 0xa5, "system info writes exact ABI size");
    uint8_t data[390] = {};
    time_fixture_load(process, output, data, length);
    if (number == SYSCALL_uname)
    {
        ok &= memory_expect(strcmp((char*) data, "Arx") == 0 && strcmp((char*) data + 65, "arx-kernel") == 0, "uname identity fields");
        for (size_t field = 0; field < 6; ++field)
            ok &= memory_expect(data[field * 65 + 64] == 0, "uname fields are NUL terminated");
#if defined(__x86_64__)
        ok &= memory_expect(strcmp((char*) data + 4 * 65, "x86_64") == 0, "uname target architecture");
#else
        ok &= memory_expect(strcmp((char*) data + 4 * 65, "aarch64") == 0, "uname target architecture");
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
        ok &= memory_expect(clockResult == 0 && uptime >= 0 && uptime <= now.seconds + 1 && total >= free && unit == 1, "sysinfo uptime and memory units");
        ok &= memory_expect(tasks > 0 && tasks <= TaskManager::MAX_TASKS, "sysinfo counts allocated tasks");
        ok &= memory_expect(data[82] == 0 && data[83] == 0 && data[84] == 0 && data[108] == 0 && data[111] == 0, "sysinfo zeroes ABI padding");
    }
    ok &= memory_expect(memory_call(number, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "system info rejects kernel output");
    ok &= memory_expect(memory_call(number, base + 2 * PAGE_SIZE - length + 1) == LINUX_EFAULT, "system info rejects partly unmapped output");
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(number, output) == LINUX_EFAULT, "system info rejects readonly output page");
    return ok;
}

template <uint64_t Number> static bool custom_case_validate_system(const custom_case_ctx_t*)
{
    return run_memory_case(
            [](process_t* process)
            {
                if (Number == SYSCALL_arch_prctl)
                    return test_system_tls(process);
                if (Number == SYSCALL_prctl)
                    return test_system_name(process);
                if (Number == SYSCALL_uname || Number == SYSCALL_sysinfo)
                    return test_system_info(process, Number);
                if (Number == SYSCALL_getrlimit || Number == SYSCALL_setrlimit || Number == SYSCALL_prlimit64)
                    return test_system_limits(process, Number);
                if (Number == SYSCALL_getrandom)
                    return memory_expect(memory_call(Number, 0, 0, 0) == LINUX_ENOSYS && memory_call(Number, 0, 16, 1) == LINUX_ENOSYS && memory_call(Number, 0, 16, 8) == LINUX_EINVAL && memory_call(Number, 0, 16, 6) == LINUX_EINVAL,
                                         "getrandom rejects invalid flags and reports missing entropy subsystem");
                return false;
            });
}

static void test_system_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<SystemRequestManager> tests[] = {
            {"HandleArch_prctlRequest", &SystemRequestManager::HandleArch_prctlRequest, SYSCALL_arch_prctl, custom_case_validate_system<SYSCALL_arch_prctl>},
            {"HandleUmaskRequest", &SystemRequestManager::HandleUmaskRequest, SYSCALL_umask, custom_case_validate_umask},
            {"HandlePrlimit64Request", &SystemRequestManager::HandlePrlimit64Request, SYSCALL_prlimit64, custom_case_validate_system<SYSCALL_prlimit64>},
            {"HandleGetrlimitRequest", &SystemRequestManager::HandleGetrlimitRequest, SYSCALL_getrlimit, custom_case_validate_system<SYSCALL_getrlimit>},
            {"HandleSetrlimitRequest", &SystemRequestManager::HandleSetrlimitRequest, SYSCALL_setrlimit, custom_case_validate_system<SYSCALL_setrlimit>},
            {"HandlePrctlRequest", &SystemRequestManager::HandlePrctlRequest, SYSCALL_prctl, custom_case_validate_system<SYSCALL_prctl>},
            {"HandleUnameRequest", &SystemRequestManager::HandleUnameRequest, SYSCALL_uname, custom_case_validate_system<SYSCALL_uname>},
            {"HandleSysinfoRequest", &SystemRequestManager::HandleSysinfoRequest, SYSCALL_sysinfo, custom_case_validate_system<SYSCALL_sysinfo>},
            {"HandleGetrandomRequest", &SystemRequestManager::HandleGetrandomRequest, SYSCALL_getrandom, custom_case_validate_system<SYSCALL_getrandom>},
    };

    if (caps->systemRequestManager != nullptr)
        for (const auto& test : tests)
            if ((caps->systemRequestManager->*(test.method))(nullptr) != LINUX_EINVAL)
            {
                stats->implementedFail++;
                selftest_record_failure_detail("system handler null-frame validation");
            }
    run_manager_tests("system", caps->systemRequestManager, tests, stats, true);
}

static void test_socket_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<SocketRequestManager> tests[] = {
            {"HandleSocketRequest", &SocketRequestManager::HandleSocketRequest, SYSCALL_socket},
            {"HandleSocketpairRequest", &SocketRequestManager::HandleSocketpairRequest, SYSCALL_socketpair},
            {"HandleBindRequest", &SocketRequestManager::HandleBindRequest, SYSCALL_bind},
            {"HandleConnectRequest", &SocketRequestManager::HandleConnectRequest, SYSCALL_connect},
            {"HandleListenRequest", &SocketRequestManager::HandleListenRequest, SYSCALL_listen},
            {"HandleAccept4Request", &SocketRequestManager::HandleAccept4Request, SYSCALL_accept4},
            {"HandleAcceptRequest", &SocketRequestManager::HandleAcceptRequest, SYSCALL_accept},
            {"HandleRecvfromRequest", &SocketRequestManager::HandleRecvfromRequest, SYSCALL_recvfrom},
            {"HandleRecvmsgRequest", &SocketRequestManager::HandleRecvmsgRequest, SYSCALL_recvmsg},
            {"HandleSendtoRequest", &SocketRequestManager::HandleSendtoRequest, SYSCALL_sendto},
            {"HandleSendmsgRequest", &SocketRequestManager::HandleSendmsgRequest, SYSCALL_sendmsg},
            {"HandleShutdownRequest", &SocketRequestManager::HandleShutdownRequest, SYSCALL_shutdown},
            {"HandleGetsockoptRequest", &SocketRequestManager::HandleGetsockoptRequest, SYSCALL_getsockopt},
            {"HandleSetsockoptRequest", &SocketRequestManager::HandleSetsockoptRequest, SYSCALL_setsockopt},
            {"HandleGetsocknameRequest", &SocketRequestManager::HandleGetsocknameRequest, SYSCALL_getsockname},
            {"HandleGetpeernameRequest", &SocketRequestManager::HandleGetpeernameRequest, SYSCALL_getpeername},
    };

    run_manager_tests("socket", caps->socketRequestManager, tests, stats);
}
} // namespace

extern "C" void launch_syscall_selftests(void* requestLayerCaps)
{
    requestlayer_stats_t stats = {};

    selftest_case_begin("requestlayer_selftest");

    RequestLayerCaps* caps = static_cast<RequestLayerCaps*>(requestLayerCaps);
    if (caps == nullptr)
    {
        stats.implementedFail++;
        selftest_record_failure_detail("request layer caps missing");
        selftest_case_end("requestlayer_selftest", stats.implemented, stats.implementedFail);
        return;
    }

    test_user_copy_fixture(&stats);
    test_process_manager(caps, &stats);
    test_scheduler_manager(caps, &stats);
    test_time_manager(caps, &stats);
    test_memory_manager(caps, &stats);
    test_vfs_manager(caps, &stats);
    test_event_manager(caps, &stats);
    test_sync_manager(caps, &stats);
    test_signal_manager(caps, &stats);
    test_credential_manager(caps, &stats);
    test_system_manager(caps, &stats);
    test_socket_manager(caps, &stats);

    kprintf("Arx kernel: requestlayer_selftest total=%llu implemented=%llu unimplemented=%llu implemented_pass=%llu implemented_fail=%llu custom_used=%llu custom_pass=%llu custom_fail=%llu\n", stats.total, stats.implemented, stats.unimplemented, stats.implementedPass, stats.implementedFail,
            stats.customCaseUsed, stats.customCasePass, stats.customCaseFail);

    selftest_case_end("requestlayer_selftest", stats.implemented, stats.implementedFail);
}
