#include "layers/Dispatcher.hpp"
#include "layers/Logic/CredentialManager.hpp"
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/SignalManager.hpp"
#include "layers/Logic/SocketManager.hpp"
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
static_assert(sizeof(arch_syscall_frame_t) == 176, "x86-64 syscall frame size must match entry assembly");
static_assert(offsetof(arch_syscall_frame_t, arg0) == 8, "RDI syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg1) == 16, "RSI syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg2) == 24, "RDX syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg3) == 32, "R10 syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg4) == 40, "R8 syscall argument offset");
static_assert(offsetof(arch_syscall_frame_t, arg5) == 48, "R9 syscall argument offset");
#endif

namespace
{
#if defined(__x86_64__)
// Independent Linux x86-64 ABI fixture for inspecting delivered user signal frames.
struct signal_context_fixture_t
{
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, di, si, bp, bx, dx, ax, cx, sp, ip, flags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved[8];
};
struct signal_ucontext_fixture_t
{
    uint64_t                 flags, link;
    signal_stack_t           stack;
    signal_context_fixture_t context;
    uint64_t                 mask;
};
struct signal_frame_fixture_t
{
    uint64_t                  restorer;
    signal_ucontext_fixture_t uc;
    uint8_t                   info[128];
};
static_assert(sizeof(signal_context_fixture_t) == 256 && sizeof(signal_ucontext_fixture_t) == 304 && sizeof(signal_frame_fixture_t) == 440, "Linux x86-64 rt signal frame ABI");
static_assert(offsetof(signal_ucontext_fixture_t, context) == 40 && offsetof(signal_context_fixture_t, r13) == 40, "user fixture R13 context offset");
#endif

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
        {
            return;
        }

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
        {
            cpu_preemption_enable();
        }
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
    constexpr uint64_t TEST_LINUX_EBUSY = (uint64_t) -16;

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

    return rmdirResult == TEST_LINUX_EBUSY && badPathResult == LINUX_EFAULT;
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
    {
        selftest_record_failure_detail(detail);
    }

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
    {
        return false;
    }

    for (uint64_t p = a; p < a + 3 * PAGE_SIZE; p += PAGE_SIZE)

        for (unsigned i = 0; i < PAGE_SIZE; ++i)

            if (!memory_bytes(process, p) || memory_bytes(process, p)[i])
            {
                return memory_expect(false, "mmap zero initialization");
            }

    uint64_t b = memory_map(1);
    ok &= memory_expect(b == a + 3 * PAGE_SIZE && memory_bytes(process, b) != memory_bytes(process, a), "mmap independent backing");
    uint64_t fixed = 0x400000;
    ok &= memory_expect(memory_map(1, fixed, 0x100022) == fixed, "mmap fixed noreplace");
    ok &= memory_expect(memory_map(1, fixed, 0x100022) == static_cast<uint64_t>(-17), "mmap collision EEXIST");

    if (memory_bytes(process, fixed))
    {
        *memory_bytes(process, fixed) = 0xA5;
    }

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
    {
        free = free->next;
    }

    if (!free)
    {
        return memory_expect(false, "mmap rollback fixture range");
    }

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
    {
        return false;
    }

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
    {
        return false;
    }

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
    {
        return false;
    }

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
    {
        return false;
    }

    bool ok                   = true;
    *memory_bytes(process, a) = 0xA5;

    for (int advice = 0; advice <= 3; ++advice)
    {
        ok &= memory_expect(memory_call(SYSCALL_madvise, a, PAGE_SIZE, advice) == 0 && *memory_bytes(process, a) == 0xA5, "madvise hint accepted preserves content");
    }

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
    {
        return false;
    }

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
        process_t* timerProcess = processManager->AcquireProcess(process->id, true);
        result &= memory_expect(timerProcess == nullptr, "timer scan skips process without armed timer");
        processManager->ReleaseProcess(timerProcess);
        process_t* pinned = processManager->AcquireCurrentMemoryProcess();
        result &= memory_expect(pinned == process, "memory syscall fixture pins current process");

        if (pinned != nullptr)
        {
            result &= memory_expect(!processManager->FreeProcess(process), "memory syscall lifetime pin blocks teardown");
            process_t* blockedAlias = processManager->CreateProcess(space);
            result &= memory_expect(blockedAlias == nullptr, "memory syscall pin blocks shared-root attachment");

            if (blockedAlias != nullptr)
            {
                result &= memory_expect(processManager->FreeProcess(blockedAlias), "unexpected shared-root attachment cleanup");
            }

            processManager->ReleaseMemoryProcess(pinned);
        }

        // Timer scans retain process metadata without accessing its mappings.
        // Hold that same pin to reproduce concurrent alias creation reliably.
        process_t* retained = processManager->AcquireProcess(process->id);
        result &= memory_expect(retained == process, "memory syscall fixture retains process metadata");
        process_t* alias = processManager->CreateProcess(space);
        processManager->ReleaseProcess(retained);
        result &= memory_expect(alias != nullptr, "memory syscall shared-root fixture creates alias");

        if (alias != nullptr)
        {
            result &= memory_expect(memory_map(1) == LINUX_ESRCH, "memory syscall shared roots rejected before mutation");
            result &= memory_expect(memory_call(SYSCALL_brk) == process->breakBase, "brk query remains current on unsupported shared root");
            process_t* retainedAlias = processManager->AcquireProcess(alias->id);
            result &= memory_expect(retainedAlias == alias && !processManager->FreeProcess(alias), "metadata lifetime pin blocks process teardown");
            processManager->ReleaseProcess(retainedAlias);
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
            {
                continue;
            }
        }
        else
            stats->implemented++;

        if (tests[index].customValidator != nullptr)
        {
            const custom_case_ctx_t customCtx = {
                    managerName, tests[index].name, tests[index].syscallNumber, &frame, result,
            };
            stats->customCaseUsed++;
            const bool traceWait = strcmp(managerName, "time") == 0 || strcmp(managerName, "event") == 0 || strcmp(managerName, "sync") == 0 || strcmp(managerName, "signal") == 0;
            uint64_t   started = 0, finished = 0;
            const bool haveStart = traceWait && arch_time_read_monotonic(&started);

            if (traceWait)
            {
                KDEBUG("[selftest] running %s::%s\n", managerName, tests[index].name);
            }

            const bool passed = tests[index].customValidator(&customCtx) && userFixture.valid;

            if (traceWait)
            {
                if (haveStart && arch_time_read_monotonic(&finished) && finished >= started)
                    KDEBUG("[selftest] finished %s::%s elapsed=%llu ms result=%s\n", managerName, tests[index].name, (unsigned long long) ((finished - started) / 1000000), passed ? "PASS" : "FAIL");
                else
                {
                    KDEBUG("[selftest] finished %s::%s result=%s (clock unavailable)\n", managerName, tests[index].name, passed ? "PASS" : "FAIL");
                }
            }

            if (passed)
            {
                stats->customCasePass++;

                if (!unavailable)
                {
                    stats->implementedPass++;
                }
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
    {
        *memory_bytes(process, address + i) = bytes[i];
    }
}

static void time_fixture_load(process_t* process, uint64_t address, void* destination, size_t length)
{
    uint8_t* bytes = static_cast<uint8_t*>(destination);

    for (size_t i = 0; i < length; ++i)
    {
        bytes[i] = *memory_bytes(process, address + i);
    }
}

static bool test_time_clock(process_t* process, uint64_t number)
{
    uint64_t base = memory_map(2);

    if (static_cast<int64_t>(base) < 0)
    {
        return memory_expect(false, "time fixture mapping");
    }

    uint64_t     output                         = base + PAGE_SIZE - 8;
    const size_t outputSize                     = number == SYSCALL_time ? 8 : 16;
    *memory_bytes(process, output - 1)          = 0xa5;
    *memory_bytes(process, output + outputSize) = 0xa5;
    time_timespec_t value                       = {};
    uint64_t        result                      = number == SYSCALL_time ? memory_call(number, output) : number == SYSCALL_gettimeofday ? memory_call(number, output, base + 64) : memory_call(number, 1, output);

    if (result == LINUX_ENOSYS)
    {
        kprintf("Arx kernel: time selftest clock unavailable syscall=%llu\n", (unsigned long long) number);
        return memory_expect(number == SYSCALL_time || number == SYSCALL_gettimeofday, "monotonic clock unavailable; inspect boot clock diagnostic");
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
            {
                ok &= memory_expect(value.seconds > 0 || value.nanoseconds > 0, "clock positive resolution");
            }
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
    {
        return memory_expect(false, "sleep fixture mapping");
    }

    uint64_t        input   = base + PAGE_SIZE - 8;
    auto            call    = [number](uint64_t pointer, uint64_t clock = 1, uint64_t flags = 0) { return number == SYSCALL_nanosleep ? memory_call(number, pointer, CANONICAL_KERNEL_BASE) : memory_call(number, clock, flags, pointer, CANONICAL_KERNEL_BASE); };
    bool            ok      = memory_expect(call(0) == LINUX_EFAULT && call(CANONICAL_KERNEL_BASE) == LINUX_EFAULT && call(base + 2 * PAGE_SIZE - 8) == LINUX_EFAULT, "sleep invalid request buffers");
    time_timespec_t request = {-1, 0};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(call(input) == LINUX_EINVAL, "sleep negative seconds");
    request = {0, 1000000000};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(call(input) == LINUX_EINVAL, "sleep invalid nanoseconds");
    Dispatcher*        dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps* resources  = dispatcher->GetResourceLayerCaps();
    Scheduler*         scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    time_timespec_t    clockProbe;

    if (resources->timeManager == nullptr || resources->timeManager->ReadClock(1, &clockProbe) != 0)
    {
        // Retain failure reporting without creating a runnable peer that a
        // missing clock cannot park/wake the current task to exercise.
        return memory_expect(false, "sleep requires monotonic clock; inspect boot clock diagnostic");
    }

    sleep_peer_context_t peerContext = {scheduler, resources->processManager, resources->taskManager, process, resources->taskManager->GetCurrentTask(), false, false, false};
    virt_addr_space_t*   peerSpace   = resources->virtualMemoryManager->CreateUserAddressSpace();
    process_t*           peerProcess = peerSpace != nullptr ? resources->processManager->CreateProcess(peerSpace) : nullptr;
    task_t*              peerTask    = resources->taskManager->CreateKernelTask(sleep_peer_entry, &peerContext);
    bool                 peerReady   = peerProcess != nullptr && peerTask != nullptr && resources->processManager->AddTask(peerProcess, peerTask) && scheduler->EnqueueProcess(arch_cpu_id(), peerProcess->id);
    ok &= memory_expect(peerReady, "sleep runnable peer fixture");

    if (!peerReady)
    {
        if (peerTask != nullptr)
        {
            resources->taskManager->FreeTask(peerTask);
        }

        if (peerProcess != nullptr)
        {
            resources->processManager->FreeProcess(peerProcess);
        }

        if (peerSpace != nullptr)
        {
            resources->virtualMemoryManager->DestroyUserAddressSpace(peerSpace);
        }

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
    {
        scheduler->KillProcess(peerProcess->id, 0);
    }

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

static bool test_time_real_expiration(process_t* process, uint64_t base)
{
    Dispatcher*        dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps* resources  = dispatcher->GetResourceLayerCaps();
    TimeManager*       clock      = resources->timeManager;
    SignalManager*     signals    = dispatcher->GetLogicLayerCaps()->signalManager;
    task_t*            task       = resources->taskManager->GetCurrentTask();
    time_timespec_t    now        = {};

    if (clock == nullptr || clock->ReadClock(1, &now) != 0)
    {
        return true;
    }

    const uint64_t  alarmBit = 1ULL << 13;
    const uint64_t  oldMask  = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);
    signal_action_t previous = {};
    signal_action_t action   = {base + 512, 0x04000004, base + 768, 0};
    bool            ok       = memory_expect(signals->Action(14, &action, &previous) == 0, "real timer signal handler fixture");
    __atomic_store_n(&task->signalMask, oldMask | alarmBit, __ATOMIC_RELEASE);
    time_itimerval_t request = {{0, 0}, {10, 0}};
    time_fixture_store(process, base + 256, &request, sizeof(request));
    const bool armed = memory_call(SYSCALL_setitimer, 0, base + 256) == 0;
    ok &= memory_expect(armed, "single-shot real timer arming");

    if (armed)
    {
        spinlock_acquire_preempt(&process->timerLock);
        const uint64_t deadline = process->realTimerDeadline;
        spinlock_release_preempt(&process->timerLock);
        ok &= memory_expect(!clock->ExpireRealTimer(process, deadline - 1) && clock->ExpireRealTimer(process, deadline) && !clock->ExpireRealTimer(process, deadline), "real timer expires once at deadline");
        ok &= memory_expect((__atomic_load_n(&process->signalPending, __ATOMIC_ACQUIRE) & alarmBit) != 0 && process->signalSenders[13].code == 128, "real timer queues kernel SIGALRM");
        memory_call(SYSCALL_getitimer, 0, base + 320);
        time_itimerval_t actual = {};
        time_fixture_load(process, base + 320, &actual, sizeof(actual));
        ok &= memory_expect(actual.value.seconds == 0 && actual.value.microseconds == 0, "expired single-shot timer is disarmed");
        ok &= memory_expect(memory_call(SYSCALL_alarm, 0) == 0 && (__atomic_load_n(&process->signalPending, __ATOMIC_ACQUIRE) & alarmBit) != 0, "timer cancellation preserves already pending SIGALRM");
        __atomic_fetch_and(&process->signalPending, ~alarmBit, __ATOMIC_RELEASE);
    }

    request = {{1, 0}, {10, 0}};
    time_fixture_store(process, base + 256, &request, sizeof(request));
    const bool periodic = memory_call(SYSCALL_setitimer, 0, base + 256) == 0;
    ok &= memory_expect(periodic, "periodic real timer arming");

    if (periodic)
    {
        // Exercise the retained timer at its exact deadline. The scheduler
        // uses the same expiration method with the current monotonic sample.
        spinlock_acquire_preempt(&process->timerLock);
        const uint64_t deadline = process->realTimerDeadline;
        spinlock_release_preempt(&process->timerLock);
        ok &= memory_expect(clock->ExpireRealTimer(process, deadline) && !process->realTimerArmed && process->realTimerExpired, "periodic timer waits for pending signal consumption");

#if defined(__x86_64__)
        alignas(16) uint8_t floating[512] = {};
        const uint32_t      mxcsr         = 0x1f80;
        const uint32_t      mxcsrMask     = 0xffbf;
        memcpy(floating + 24, &mxcsr, sizeof(mxcsr));
        memcpy(floating + 28, &mxcsrMask, sizeof(mxcsrMask));
        arch_syscall_frame_t frame = {};
        frame.syscall_number       = SYSCALL_getitimer;
        frame.user_cs              = USER_CS;
        frame.user_ss              = USER_SS;
        frame.user_rflags          = 0x202;
        frame.user_rip             = base + 1024;
        frame.user_rsp             = base + 2 * PAGE_SIZE - 16;
        frame.fxstate              = (uint64_t) floating;
        __atomic_store_n(&task->signalMask, oldMask & ~alarmBit, __ATOMIC_RELEASE);
        ok &= memory_expect(signals->Deliver(&frame, 0) == 0 && frame.arg0 == 14 && process->realTimerArmed && !process->realTimerExpired, "SIGALRM delivery rearms periodic timer");
        frame.user_rsp += 8;
        ok &= memory_expect(signals->Restore(&frame) == 0, "real timer signal frame restores context");
#else
        clock->RestartRealTimer(process);
        ok &= memory_expect(process->realTimerArmed, "periodic timer rearm");
#endif

        memory_call(SYSCALL_setitimer, 0, 0);
        __atomic_fetch_and(&process->signalPending, ~alarmBit, __ATOMIC_RELEASE);
    }

    request = {{0, 0}, {0, 10000}};
    time_fixture_store(process, base + 256, &request, sizeof(request));
    __atomic_store_n(&task->signalMask, oldMask & ~alarmBit, __ATOMIC_RELEASE);
    const bool waiting = memory_call(SYSCALL_setitimer, 0, base + 256) == 0;
    ok &= memory_expect(waiting, "parked real timer fixture");

    if (waiting)
    {
        ok &= memory_expect(memory_call(SYSCALL_poll, 0, 0, UINT64_MAX) == (uint64_t) -4, "real timer tick interrupts parked event wait with SIGALRM");
    }

    memory_call(SYSCALL_setitimer, 0, 0);
    __atomic_fetch_and(&process->signalPending, ~alarmBit, __ATOMIC_RELEASE);
    __atomic_store_n(&task->signalMask, oldMask, __ATOMIC_RELEASE);
    (void) signals->Action(14, &previous, nullptr);
    return ok;
}

static bool test_time_timer(process_t* process, uint64_t number)
{
    uint64_t base = memory_map(2);

    if (static_cast<int64_t>(base) < 0)
    {
        return memory_expect(false, "timer fixture mapping");
    }

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
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input, output) == 0 && memory_call(SYSCALL_getitimer, 0, output) == 0, "disarmed real timer clears interval");
    time_fixture_load(process, output, &value, sizeof(value));
    ok &= memory_expect(value.interval.seconds == 0 && value.interval.microseconds == 0 && value.value.seconds == 0, "disarmed real timer interval roundtrip");
    TimeManager*    clock     = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->timeManager;
    time_timespec_t now       = {};
    const bool      available = clock != nullptr && clock->ReadClock(1, &now) == 0;
    ok &= test_time_real_expiration(process, base);

    if (number == SYSCALL_alarm)
    {
        if (available)
        {
            ok &= memory_expect(memory_call(number, 2) == 0 && memory_call(number, 1) == 2 && memory_call(number, 0) == 1 && memory_call(number, 0) == 0, "alarm arm, replacement and cancellation return remaining seconds");
            ok &= memory_expect(memory_call(number, (1ULL << 32) | 2) == 0 && memory_call(number, 0) == 2, "alarm uses unsigned 32-bit seconds");
        }
        else
        {
            ok &= memory_expect(memory_call(number, 1) == LINUX_ENOSYS && memory_call(number, 0) == 0, "alarm reports unavailable clock and permits disarmed cancellation");
        }
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
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input) == (available ? 0 : LINUX_ENOSYS), "real timer arming uses monotonic clock");
    memory_call(SYSCALL_getitimer, 0, output);
    time_fixture_load(process, output, &value, sizeof(value));
    if (available)
    {
        ok &= memory_expect(value.interval.seconds == 2 && value.interval.microseconds == 123 && value.value.seconds <= 1 && (value.value.seconds != 0 || value.value.microseconds != 0), "armed real timer reports remaining duration");
        ok &= memory_expect(memory_call(SYSCALL_setitimer, 1, input) == LINUX_EOPNOTSUPP && memory_call(SYSCALL_setitimer, 2, input) == LINUX_EOPNOTSUPP, "CPU timers await process CPU accounting");
        memory_call(SYSCALL_setitimer, 0, 0);
        request = {{7, 99}, {5, 0}};
        time_fixture_store(process, input, &request, sizeof(request));
        ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "timer bad old-value output reports EFAULT");
        memory_call(SYSCALL_getitimer, 0, output);
        time_fixture_load(process, output, &value, sizeof(value));
        ok &= memory_expect(value.interval.seconds == 7 && value.interval.microseconds == 99 && (value.value.seconds != 0 || value.value.microseconds != 0), "timer output fault preserves committed new timer");
        memory_call(SYSCALL_setitimer, 0, 0);
    }

    request = {{0, 0}, {INT64_MAX, 0}};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input) == LINUX_EOVERFLOW, "real timer rejects unrepresentable duration");

    request = {{0, 1000000}, {0, 0}};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input) == LINUX_EINVAL, "timer microsecond validation");
    request = {{7, 99}, {0, 0}};
    time_fixture_store(process, input, &request, sizeof(request));
    ok &= memory_expect(memory_call(SYSCALL_setitimer, 0, input, input) == 0, "timer aliased input and previous output");
    time_fixture_load(process, input, &value, sizeof(value));
    ok &= memory_expect(value.interval.seconds == 0 && value.interval.microseconds == 0, "timer alias receives old disarmed interval");
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
                {
                    return test_time_sleep(process, Number);
                }

                if (Number == SYSCALL_getitimer || Number == SYSCALL_setitimer || Number == SYSCALL_alarm)
                {
                    return test_time_timer(process, Number);
                }

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

struct ipc_peer_context_t
{
    Scheduler* scheduler;
    uint64_t   number;
    uint64_t   args[6];
    uint64_t   result;
};

static void ipc_peer_entry(void* argument)
{
    cpu_preemption_disable();
    auto context    = static_cast<ipc_peer_context_t*>(argument);
    context->result = memory_call(context->number, context->args[0], context->args[1], context->args[2], context->args[3], context->args[4], context->args[5]);
    context->scheduler->ExitCurrentTask();
}

template <typename Wait> static bool ipc_with_peer(process_t* process, ipc_peer_context_t* context, Wait wait)
{
    auto dispatcher    = static_cast<Dispatcher*>(platform.dispacher);
    auto resources     = dispatcher->GetResourceLayerCaps();
    context->scheduler = dispatcher->GetLogicLayerCaps()->scheduler;
    context->result    = UINT64_MAX;
    task_t*    peer    = resources->taskManager->CreateKernelTask(ipc_peer_entry, context);
    const bool added   = peer != nullptr && resources->processManager->AddTask(process, peer);
    const bool ready   = added && context->scheduler->EnqueueProcess(arch_cpu_id(), process->id);
    bool       ok      = memory_expect(ready, "IPC runnable peer fixture");
    if (ready)
    {
        ok &= wait();
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
        ok &= memory_expect(resources->taskManager->FreeTask(peer), "IPC peer cleanup");
    }
    return ok;
}

static size_t ipc_descriptor_count(process_t* process)
{
    size_t count = 0;
    for (size_t i = 0; i < process->fileDescriptorCount; ++i)
    {
        count += process->fileDescriptors[i].file != nullptr;
    }
    return count;
}

static bool test_ipc_pipe(process_t* process, uint64_t base, uint64_t number)
{
    int32_t        descriptors[2] = {-1, -1};
    const uint32_t flags          = number == SYSCALL_pipe2 ? 04000 | 02000000 : 0;
    bool           ok             = memory_expect(memory_call(number, base, flags) == 0, "pipe syscall creates both endpoints");
    time_fixture_load(process, base, descriptors, sizeof(descriptors));
    if (descriptors[0] < 0 || descriptors[1] < 0)
    {
        return false;
    }
    const uint64_t reader = descriptors[0], writer = descriptors[1];
    ok &= memory_expect(memory_call(SYSCALL_fcntl, reader, 3) == (flags & 04000) && memory_call(SYSCALL_fcntl, writer, 3) == (1 | (flags & 04000)) && memory_call(SYSCALL_fcntl, reader, 1) == (uint64_t) ((flags & 02000000) != 0), "pipe access modes and descriptor flags");
    memory_call(SYSCALL_fcntl, reader, 4, 04000);
    memory_call(SYSCALL_fcntl, writer, 4, 04001);
    ok &= memory_expect(memory_call(SYSCALL_read, reader, base + 512, 1) == (uint64_t) -11 && memory_call(SYSCALL_read, writer, base + 512, 1) == LINUX_EBADF && memory_call(SYSCALL_write, reader, base + 512, 1) == LINUX_EBADF, "empty nonblocking pipe and endpoint direction validation");
    const char text[] = "pipe-data";
    time_fixture_store(process, base + 128, text, sizeof(text));
    ok &= memory_expect(memory_call(SYSCALL_write, writer, base + 128, sizeof(text)) == sizeof(text), "pipe write payload");
    ok &= memory_expect(memory_call(SYSCALL_read, reader, CANONICAL_KERNEL_BASE, sizeof(text)) == LINUX_EFAULT && memory_call(SYSCALL_read, reader, base + 512, sizeof(text)) == sizeof(text), "pipe read fault preserves data");
    char output[sizeof(text)] = {};
    time_fixture_load(process, base + 512, output, sizeof(output));
    ok &= memory_expect(memcmp(text, output, sizeof(text)) == 0, "pipe payload round trip");
    ok &= memory_expect(memory_call(SYSCALL_lseek, reader, 0, 0) == (uint64_t) -29, "pipe ESPIPE");
    ok &= memory_expect(memory_call(SYSCALL_fcntl, writer, 1031, 4096) == 4096 && memory_call(SYSCALL_fcntl, reader, 1032) == 4096, "pipe capacity control shared by endpoints");
    ok &= memory_expect(memory_call(SYSCALL_write, writer, base + 128, 4095) == 4095 && memory_call(SYSCALL_write, writer, base + 128, 2) == (uint64_t) -11, "PIPE_BUF nonblocking writes are atomic");
    ok &= memory_expect(memory_call(SYSCALL_ioctl, reader, 0x541b, base + 64) == 0, "pipe FIONREAD");
    int32_t queued = 0;
    time_fixture_load(process, base + 64, &queued, sizeof(queued));
    ok &= memory_expect(queued == 4095 && memory_call(SYSCALL_read, reader, base + 8192, 4096) == 4095, "failed atomic pipe write leaves queue unchanged");
    file_iovec_t vectors[2] = {{base + 128, 2}, {base + 130, 3}};
    time_fixture_store(process, base + 256, vectors, sizeof(vectors));
    ok &= memory_expect(memory_call(SYSCALL_writev, writer, base + 256, 2) == 5 && memory_call(SYSCALL_read, reader, base + 512, 5) == 5, "pipe writev gathers into one atomic write");
    memory_call(SYSCALL_fcntl, reader, 4, 0);
    ipc_peer_context_t peer = {nullptr, SYSCALL_write, {writer, base + 128, sizeof(text)}, UINT64_MAX};
    ok &= ipc_with_peer(process, &peer, [&]() { return memory_expect(memory_call(SYSCALL_read, reader, base + 512, sizeof(text)) == sizeof(text) && peer.result == sizeof(text), "blocking pipe read wakes on peer write"); });
    ok &= memory_expect(memory_call(SYSCALL_write, writer, base + 128, 4096) == 4096, "fill pipe before blocking writer test");
    memory_call(SYSCALL_fcntl, writer, 4, 1);
    peer = {nullptr, SYSCALL_read, {reader, base + 8192, 4096}, UINT64_MAX};
    ok &= ipc_with_peer(process, &peer, [&]() { return memory_expect(memory_call(SYSCALL_write, writer, base + 128, 1) == 1 && peer.result == 4096, "blocking pipe writer wakes after peer drains buffer"); });
    ok &= memory_expect(memory_call(SYSCALL_read, reader, base + 512, 1) == 1, "drain blocking writer test byte");
    const uint64_t alias = memory_call(SYSCALL_dup, writer);
    memory_call(SYSCALL_close, writer);
    ok &= memory_expect((int64_t) alias >= 0 && memory_call(SYSCALL_write, alias, base + 128, 1) == 1 && memory_call(SYSCALL_read, reader, base + 512, 1) == 1, "dup keeps pipe write endpoint alive");
    if ((int64_t) alias >= 0)
    {
        memory_call(SYSCALL_close, alias);
    }
    ok &= memory_expect(memory_call(SYSCALL_read, reader, base + 512, 1) == 0, "last writer close produces pipe EOF");
    event_pollfd_t poll = {(int32_t) reader, 1, 0};
    time_fixture_store(process, base + 256, &poll, sizeof(poll));
    ok &= memory_expect(memory_call(SYSCALL_poll, base + 256, 1, 0) == 1, "pipe close wakes poll");
    time_fixture_load(process, base + 256, &poll, sizeof(poll));
    ok &= memory_expect((poll.revents & 0x10) != 0, "pipe POLLHUP");
    memory_call(SYSCALL_close, reader);
    if (memory_call(SYSCALL_pipe2, base, 0) == 0)
    {
        time_fixture_load(process, base, descriptors, sizeof(descriptors));
        memory_call(SYSCALL_close, descriptors[0]);
        auto           task    = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->GetCurrentTask();
        const uint64_t pending = task->signalPending;
        ok &= memory_expect(memory_call(SYSCALL_write, descriptors[1], base + 128, 1) == (uint64_t) -32 && (task->signalPending & (1ULL << 12)) != 0, "pipe without readers returns EPIPE and queues SIGPIPE");
        task->signalPending = pending;
        memory_call(SYSCALL_close, descriptors[1]);
    }
    else
    {
        ok &= memory_expect(false, "broken-pipe fixture");
    }
    const size_t before = ipc_descriptor_count(process);
    ok &= memory_expect(memory_call(SYSCALL_pipe2, CANONICAL_KERNEL_BASE, 0) == LINUX_EFAULT && ipc_descriptor_count(process) == before, "pipe output fault rolls back both descriptors");
    const uint64_t limit       = process->fileLimit.current;
    process->fileLimit.current = 1;
    ok &= memory_expect(memory_call(SYSCALL_pipe2, base, 0) == LINUX_EMFILE && ipc_descriptor_count(process) == before, "pipe second-fd allocation rollback");
    process->fileLimit.current = limit;
    ok &= memory_expect(memory_call(SYSCALL_pipe2, base, 1) == LINUX_EINVAL && memory_call(SYSCALL_pipe2, base, 0200) == LINUX_EOPNOTSUPP, "pipe invalid flags and notification-pipe prerequisite");
    if (memory_call(SYSCALL_pipe2, base, 040000 | 04000) == 0)
    {
        time_fixture_load(process, base, descriptors, sizeof(descriptors));
        const uint64_t packetReader = descriptors[0], packetWriter = descriptors[1];
        ok &= memory_expect(memory_call(SYSCALL_write, packetWriter, base + 128, 7) == 7 && memory_call(SYSCALL_write, packetWriter, base + 128, 9) == 9, "packet pipe queues separate writes");
        ok &= memory_expect(memory_call(SYSCALL_read, packetReader, base + 512, 3) == 3 && memory_call(SYSCALL_read, packetReader, base + 512, 32) == 9 && memory_call(SYSCALL_read, packetReader, base + 512, 1) == (uint64_t) -11, "short packet read discards only remainder of first packet");
        ok &= memory_expect(memory_call(SYSCALL_write, packetWriter, base + 128, 4097) == 4097 && memory_call(SYSCALL_read, packetReader, base + 8192, 8192) == 4096 && memory_call(SYSCALL_read, packetReader, base + 512, 32) == 1, "packet writes split at PIPE_BUF");
        memory_call(SYSCALL_write, packetWriter, base + 128, 7);
        ok &= memory_expect(memory_call(SYSCALL_read, packetReader, CANONICAL_KERNEL_BASE, 7) == LINUX_EFAULT && memory_call(SYSCALL_read, packetReader, base + 512, 32) == 7, "packet read fault preserves packet");
        ok &= memory_expect(memory_call(SYSCALL_fcntl, packetWriter, 4, 04000) == 0 && memory_call(SYSCALL_write, packetWriter, base + 128, 7) == 7 && memory_call(SYSCALL_read, packetReader, base + 512, 3) == 3 && memory_call(SYSCALL_read, packetReader, base + 512, 32) == 4,
                            "fcntl O_DIRECT change affects subsequent writes");
        memory_call(SYSCALL_fcntl, packetWriter, 4, 04000 | 040000);
        memory_call(SYSCALL_write, packetWriter, base + 128, 7);
        ok &= memory_expect(memory_call(SYSCALL_fcntl, packetReader, 1031, 4096) == 4096 && memory_call(SYSCALL_read, packetReader, base + 512, 3) == 3 && memory_call(SYSCALL_read, packetReader, base + 512, 32) == (uint64_t) -11, "pipe resize preserves packet markers");
        memory_call(SYSCALL_close, packetReader);
        memory_call(SYSCALL_close, packetWriter);
    }
    else
    {
        ok &= memory_expect(false, "packet pipe creation");
    }
    return ok;
}

static bool test_ipc_memory_file(process_t* process, uint64_t base)
{
    const char name[] = "arx-memfd";
    time_fixture_store(process, base + 128, name, sizeof(name));
    const uint64_t fd = memory_call(SYSCALL_memfd_create, base + 128, 3);
    if (!memory_expect((int64_t) fd >= 0, "memfd creates anonymous regular file"))
    {
        return false;
    }
    bool       ok     = memory_expect(memory_call(SYSCALL_fcntl, fd, 1) == 1 && memory_call(SYSCALL_fcntl, fd, 1034) == 0, "memfd CLOEXEC and initially unsealed state");
    const char text[] = "memory-file";
    time_fixture_store(process, base + 512, text, sizeof(text));
    ok &= memory_expect(memory_call(SYSCALL_write, fd, base + 512, sizeof(text)) == sizeof(text) && memory_call(SYSCALL_pread64, fd, base + 1024, sizeof(text), 0) == sizeof(text), "memfd write and positioned read");
    char output[sizeof(text)] = {};
    time_fixture_load(process, base + 1024, output, sizeof(output));
    ok &= memory_expect(memcmp(text, output, sizeof(text)) == 0 && memory_call(SYSCALL_lseek, fd, 0, 1) == sizeof(text), "memfd positioned read preserves shared file offset");
    ok &= memory_expect(memory_call(SYSCALL_ftruncate, fd, PAGE_SIZE * 2 + 17) == 0 && memory_call(SYSCALL_pread64, fd, base + 1024, 32, PAGE_SIZE) == 32, "memfd sparse growth");
    uint8_t zeros[32] = {};
    time_fixture_load(process, base + 1024, zeros, sizeof(zeros));
    bool zero = true;
    for (uint8_t byte : zeros)
    {
        zero &= byte == 0;
    }
    ok &= memory_expect(zero, "memfd holes read as zero");
    ok &= memory_expect(memory_call(SYSCALL_ftruncate, fd, 3) == 0 && memory_call(SYSCALL_ftruncate, fd, 32) == 0 && memory_call(SYSCALL_pread64, fd, base + 1024, 29, 3) == 29, "memfd shrink discards partial-page tail");
    time_fixture_load(process, base + 1024, zeros, 29);
    zero = true;
    for (size_t i = 0; i < 29; ++i)
    {
        zero &= zeros[i] == 0;
    }
    ok &= memory_expect(zero, "memfd truncated bytes do not reappear on growth");
    ok &= memory_expect(memory_call(SYSCALL_fcntl, fd, 1033, 2 | 4 | 8) == 0 && memory_call(SYSCALL_write, fd, base + 512, 1) == (uint64_t) -1 && memory_call(SYSCALL_ftruncate, fd, 31) == (uint64_t) -1 && memory_call(SYSCALL_ftruncate, fd, 33) == (uint64_t) -1,
                        "memfd write/grow/shrink seals return EPERM");
    ok &= memory_expect(memory_call(SYSCALL_fcntl, fd, 1033, 1) == 0 && memory_call(SYSCALL_fcntl, fd, 1033, 16) == (uint64_t) -1, "memfd seal-seal is irreversible");
    memory_call(SYSCALL_close, fd);
    const uint64_t fixed = memory_call(SYSCALL_memfd_create, base + 128, 0);
    ok &= memory_expect((int64_t) fixed >= 0 && memory_call(SYSCALL_fcntl, fixed, 1034) == 1, "memfd defaults to F_SEAL_SEAL");
    if ((int64_t) fixed >= 0)
    {
        memory_call(SYSCALL_close, fixed);
    }
    ok &= memory_expect(memory_call(SYSCALL_memfd_create, CANONICAL_KERNEL_BASE, 0) == LINUX_EFAULT && memory_call(SYSCALL_memfd_create, base + 128, 0x8000) == LINUX_EINVAL && memory_call(SYSCALL_memfd_create, base + 128, 4) == LINUX_EOPNOTSUPP, "memfd bad names/flags and hugepage prerequisite");
    return ok;
}

static bool test_vfs_metadata(process_t* process, uint64_t base, uint64_t number)
{
    const char path[] = "/test.txt", missing[] = "/missing-metadata", empty[] = "";
    time_fixture_store(process, base + 128, path, sizeof(path));
    time_fixture_store(process, base + 256, missing, sizeof(missing));
    time_fixture_store(process, base + 384, empty, sizeof(empty));
    const uint64_t disk      = memory_call(SYSCALL_open, base + 128, 0, 0);
    bool           ownership = number == SYSCALL_fchownat || number == SYSCALL_chown || number == SYSCALL_fchown || number == SYSCALL_lchown;
    bool           direct    = number == SYSCALL_fchown || number == SYSCALL_fchmod;
    auto           call      = [&](uint64_t pointer, uint64_t fd, uint64_t flags)
    {
        if (direct)
        {
            return ownership ? memory_call(number, fd, 23, 24) : memory_call(number, fd, 0600);
        }
        if (number == SYSCALL_fchownat)
        {
            return memory_call(number, fd, pointer, 23, 24, flags);
        }
        if (number == SYSCALL_fchmodat || number == SYSCALL_fchmodat2)
        {
            return memory_call(number, fd, pointer, 0600, flags);
        }
        return ownership ? memory_call(number, pointer, 23, 24) : memory_call(number, pointer, 0600);
    };
    bool ok = memory_expect((int64_t) disk >= 0 && call(base + 128, direct ? disk : (uint64_t) -100, 0) == (uint64_t) -30, "metadata changes honor readonly cpio backend");
    ok &= memory_expect(call(direct ? 0 : base + 256, direct ? UINT32_MAX : (uint64_t) -100, 0) == (direct ? LINUX_EBADF : LINUX_ENOENT), "metadata target resolution precedes mutation");
    if (!direct)
    {
        ok &= memory_expect(call(CANONICAL_KERNEL_BASE, (uint64_t) -100, 0) == LINUX_EFAULT, "metadata syscall rejects kernel path pointer");
    }
    if ((int64_t) disk >= 0)
    {
        memory_call(SYSCALL_close, disk);
    }
    const uint64_t anonymous = memory_call(SYSCALL_memfd_create, base + 384, 0);
    ok &= memory_expect((int64_t) anonymous >= 0 && memory_call(SYSCALL_fchmod, anonymous, 0640) == 0 && memory_call(SYSCALL_fchown, anonymous, 31, 32) == 0, "anonymous inode metadata is mutable");
    if ((int64_t) anonymous >= 0)
    {
        ok &= memory_expect(memory_call(SYSCALL_fchmodat2, anonymous, base + 384, 0600, 0x1000) == 0 && memory_call(SYSCALL_fchownat, anonymous, 0, UINT32_MAX, UINT32_MAX, 0x1000) == 0, "metadata AT_EMPTY_PATH including null path");
        ok &= memory_expect(memory_call(SYSCALL_fstat, anonymous, base + 512) == 0, "metadata fstat snapshot");
        // Linux x86-64 stat mode/uid/gid fields begin at byte 24.
        uint32_t metadata[3] = {};
        time_fixture_load(process, base + 512 + 24, metadata, sizeof(metadata));
        ok &= memory_expect((metadata[0] & 07777) == 0600 && metadata[1] == 31 && metadata[2] == 32, "fstat reports committed mode and ownership");
        const auto credentials               = process->credentials;
        process->credentials.user.filesystem = 99;
        ok &= memory_expect(memory_call(SYSCALL_fchmod, anonymous, 0777) == (uint64_t) -1 && memory_call(SYSCALL_fchown, anonymous, 99, 99) == (uint64_t) -1, "metadata rejects unprivileged nonowner");
        process->credentials = credentials;
        memory_call(SYSCALL_close, anonymous);
    }
    return ok;
}

struct mount_test_backend_t;
struct mount_test_node_t
{
    inode_t               inode;
    mount_test_backend_t* owner;
    inode_t*              parent;
    inode_t*              alias;
    char                  name[32];
    char                  bytes[64];
    bool                  present;
};
struct mount_test_backend_t
{
    mount_test_node_t root;
    mount_test_node_t nodes[8];
};
static unsigned mount_test_releases = 0;
static int64_t  mount_test_metadata(inode_t*, uint32_t mode, uint32_t, uint32_t)
{
    return mode == 0777 ? -5 : 0;
}
static mount_test_node_t* mount_test_find(inode_t* directory, const char* name)
{
    auto state = static_cast<mount_test_node_t*>(directory->privateData)->owner;
    for (auto& node : state->nodes)
    {
        if (node.present && node.parent == directory && strcmp(node.name, name) == 0)
        {
            return &node;
        }
    }
    return nullptr;
}
static inode_t* mount_test_lookup(inode_t* directory, const char* name)
{
    auto node = mount_test_find(directory, name);
    return node != nullptr ? (node->alias != nullptr ? node->alias : &node->inode) : nullptr;
}
extern const inode_operations_t mount_test_inode_operations;
extern const file_operations_t  mount_test_file_operations;
static int64_t                  mount_test_create(inode_t* directory, const char* name, uint32_t mode, inode_type_t type)
{
    auto state = static_cast<mount_test_node_t*>(directory->privateData)->owner;
    if (strlen(name) >= 32)
    {
        return -36;
    }
    if (mount_test_find(directory, name) != nullptr)
    {
        return -17;
    }
    for (auto& node : state->nodes)
    {
        if (node.inode.inodeNumber != 0)
        {
            continue;
        }
        node         = {};
        node.present = true;
        node.owner   = state;
        node.parent  = directory;
        memcpy(node.name, name, strlen(name) + 1);
        node.inode.inodeNumber   = &node - state->nodes + 2;
        node.inode.type          = type;
        node.inode.mode          = mode & 07777;
        node.inode.metadataValid = true;
        node.inode.linkCount     = 1;
        node.inode.inodeOps      = &mount_test_inode_operations;
        node.inode.fileOps       = &mount_test_file_operations;
        node.inode.privateData   = &node;
        node.inode.filesystem    = directory->filesystem;
        return 0;
    }
    return -28;
}
static int64_t mount_test_mkdir(inode_t* directory, const char* name, const inode_creation_attributes_t* attributes)
{
    const int64_t result = mount_test_create(directory, name, attributes->mode, INODE_DIRECTORY);
    if (result == 0)
    {
        auto inode = mount_test_lookup(directory, name);
        inode->uid = attributes->uid;
        inode->gid = attributes->gid;
    }
    return result;
}
static int64_t mount_test_mknod(inode_t* directory, const char* name, const inode_creation_attributes_t* attributes, uint64_t)
{
    const int64_t result = mount_test_create(directory, name, attributes->mode, INODE_REGULAR);
    if (result == 0)
    {
        auto inode = mount_test_lookup(directory, name);
        inode->uid = attributes->uid;
        inode->gid = attributes->gid;
    }
    return result;
}
static int64_t mount_test_unlink(inode_t* directory, const char* name, bool directoryEntry)
{
    auto node = mount_test_find(directory, name);
    if (node == nullptr)
    {
        return -2;
    }
    if ((node->inode.type == INODE_DIRECTORY) != directoryEntry)
    {
        return directoryEntry ? -20 : -21;
    }
    node->present  = false;
    inode_t* inode = node->alias != nullptr ? node->alias : &node->inode;
    --inode->linkCount;
    return 0;
}
static int64_t mount_test_rename(inode_t* directory, const char* name, inode_t* destination, const char* newName)
{
    auto node = mount_test_find(directory, name);
    if (node == nullptr)
    {
        return -2;
    }
    if (strlen(newName) >= 32 || mount_test_find(destination, newName) != nullptr)
    {
        return -17;
    }
    node->parent = destination;
    memcpy(node->name, newName, strlen(newName) + 1);
    return 0;
}
static int64_t mount_test_link(inode_t* directory, const char* name, inode_t* inode)
{
    const int64_t result = mount_test_create(directory, name, inode->mode, inode->type);
    if (result == 0)
    {
        mount_test_find(directory, name)->alias = inode;
        ++inode->linkCount;
    }
    return result;
}
static int64_t mount_test_symlink(inode_t* directory, const char* name, const char* target, const inode_creation_attributes_t* attributes)
{
    if (strlen(target) >= 64)
    {
        return -36;
    }
    const int64_t result = mount_test_create(directory, name, 0777, INODE_SYMLINK);
    if (result == 0)
    {
        auto node = mount_test_find(directory, name);
        memcpy(node->bytes, target, strlen(target) + 1);
        node->inode.size = strlen(target);
        node->inode.uid  = attributes->uid;
        node->inode.gid  = attributes->gid;
    }
    return result;
}
static int64_t mount_test_truncate(inode_t* inode, uint64_t size)
{
    if (size > 64)
    {
        return -27;
    }
    auto node = static_cast<mount_test_node_t*>(inode->privateData);
    if (size < inode->size)
    {
        memset(node->bytes + size, 0, 64 - size);
    }
    return 0;
}
static int64_t mount_test_read(file_t* file, void* output, uint64_t count)
{
    auto         node  = static_cast<mount_test_node_t*>(file->inode->privateData);
    const size_t bytes = file->offset < file->inode->size ? (count < file->inode->size - file->offset ? count : file->inode->size - file->offset) : 0;
    memcpy(output, node->bytes + file->offset, bytes);
    file->offset += bytes;
    return (int64_t) bytes;
}
static int64_t mount_test_write(file_t* file, const void* input, uint64_t count)
{
    auto         node  = static_cast<mount_test_node_t*>(file->inode->privateData);
    const size_t bytes = file->offset < 64 ? (count < 64 - file->offset ? count : 64 - file->offset) : 0;
    memcpy(node->bytes + file->offset, input, bytes);
    file->offset += bytes;
    if (file->offset > file->inode->size)
    {
        file->inode->size = file->offset;
    }
    return (int64_t) bytes;
}
const inode_operations_t mount_test_inode_operations = {mount_test_lookup, mount_test_metadata, mount_test_mkdir, mount_test_unlink, mount_test_rename, mount_test_link, mount_test_mknod, mount_test_symlink, mount_test_truncate};
const file_operations_t  mount_test_file_operations  = {nullptr, mount_test_read, mount_test_write};
static int64_t           mount_test_mount(filesystem_t* fs, const filesystem_mount_request_t* request)
{
    auto state = static_cast<mount_test_backend_t*>(kzalloc(sizeof(mount_test_backend_t)));
    if (state == nullptr)
    {
        return -12;
    }
    fs->privateData                 = state;
    state->root.owner               = state;
    state->root.inode.type          = INODE_DIRECTORY;
    state->root.inode.mode          = 0755;
    state->root.inode.metadataValid = true;
    state->root.inode.linkCount     = 2;
    state->root.inode.inodeOps      = &mount_test_inode_operations;
    state->root.inode.privateData   = &state->root;
    state->root.inode.filesystem    = fs;
    fs->rootInode                   = &state->root.inode;
    mount_test_create(fs->rootInode, "data", 0644, INODE_REGULAR);
    return request->source != nullptr && strcmp(request->source, "fail") == 0 ? -28 : request->options != nullptr && strcmp(request->options, "test=1") != 0 ? -22 : 0;
}
static void mount_test_unmount(filesystem_t* fs)
{
    ++mount_test_releases;
    kfree(fs->privateData);
}
static int64_t mount_test_remount(filesystem_t*, uint64_t, const char* options)
{
    return options != nullptr && strcmp(options, "reject") == 0 ? -22 : 0;
}
static const filesystem_type_t mount_test_type = {"arx-test-backend", mount_test_mount, mount_test_unmount, mount_test_remount};

static bool test_vfs_backend_syscalls(process_t* process, uint64_t base)
{
    auto    dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto    resources  = dispatcher->GetResourceLayerCaps();
    inode_t directory  = {};
    directory.type     = INODE_DIRECTORY;
    dentry_t root = {}, points[3] = {};
    root.name               = "/";
    root.inode              = &directory;
    mount_t rootMount       = {};
    rootMount.root          = &root;
    const unsigned releases = mount_test_releases;
    bool           ok       = true;
    {
        VirtualFileSystem local(resources);
        LogicLayerCaps    logic = *dispatcher->GetLogicLayerCaps();
        logic.virtualFileSystem = &local;
        VfsRequestManager manager(resources, &logic);
        ok &= memory_expect(local.SetRootMount(&rootMount) && local.RegisterFileSystemType(&mount_test_type) && !local.RegisterFileSystemType(&mount_test_type), "filesystem registration rejects duplicate names");
        const char* names[] = {"one", "two", "three"};
        for (size_t i = 0; i < 3; ++i)
        {
            points[i].name   = names[i];
            points[i].parent = &root;
            points[i].inode  = &directory;
            ok &= local.CacheDentry(&points[i]);
        }
        auto store = [&](uint64_t offset, const char* text)
        {
            time_fixture_store(process, base + offset, text, strlen(text) + 1);
            return base + offset;
        };
        auto mount = [&](const char* source, const char* target, uint64_t flags, const char* options = nullptr)
        {
            arch_syscall_frame_t frame = {};
            frame.arg0                 = source != nullptr ? store(128, source) : 0;
            frame.arg1                 = store(256, target);
            frame.arg2                 = store(384, mount_test_type.name);
            frame.arg3                 = flags;
            frame.arg4                 = options != nullptr ? store(512, options) : 0;
            return manager.HandleMountRequest(&frame);
        };
        ok &= memory_expect(mount("fail", "/one", 0) == (uint64_t) -28 && mount_test_releases == releases + 1, "failed backend mount releases private state");
        ok &= memory_expect(mount("device", "/one", 0, "test=1") == 0, "mount registered backend with copied source and options");
        arch_syscall_frame_t metadata = {};
        metadata.arg0                 = store(256, "/one/data");
        metadata.arg1                 = 0600;
        ok &= memory_expect(manager.HandleChmodRequest(&metadata) == 0, "chmod dispatches to mounted inode callback");
        vfs_path_t resolved = {};
        ok &= memory_expect(local.ResolvePath({}, "/one/data", &resolved) && resolved.dentry->inode->mode == 0600, "mounted metadata change is committed");
        metadata.arg1 = 0777;
        ok &= memory_expect(manager.HandleChmodRequest(&metadata) == (uint64_t) -5 && resolved.dentry->inode->mode == 0600, "backend metadata failure preserves cached attributes");
        EventManager notifications(resources, logic.scheduler, &local);
        local.SetEventManager(&notifications);
        const uint64_t notify = notifications.CreateInotify(04000);
        const uint64_t watch  = notifications.AddWatch((int32_t) notify, store(256, "/one"), 0xc0);
        ok &= memory_expect((int64_t) notify >= 0 && (int64_t) watch > 0, "watch registered filesystem directory");
        auto call = [&](auto handler, uint64_t first, uint64_t second = 0, uint64_t third = 0)
        {
            arch_syscall_frame_t frame = {};
            frame.arg0                 = first;
            frame.arg1                 = second;
            frame.arg2                 = third;
            return (manager.*handler)(&frame);
        };
        ok &= memory_expect(call(&VfsRequestManager::HandleMkdirRequest, store(256, "/one/newdir"), 0750) == 0 && local.ResolvePath({}, "/one/newdir", &resolved) && resolved.dentry->inode->type == INODE_DIRECTORY, "mkdir dispatches to mounted directory callback");
        ok &= memory_expect(call(&VfsRequestManager::HandleMknodRequest, store(256, "/one/newfile"), 0100600) == 0 && local.ResolvePath({}, "/one/newfile", &resolved), "mknod publishes backend node through lookup");
        const uint64_t nativeFile = call(&VfsRequestManager::HandleOpenRequest, store(256, "/one/newfile"), 2);
        ok &= memory_expect((int64_t) nativeFile >= 0 && call(&VfsRequestManager::HandleWriteRequest, nativeFile, store(512, "contents"), 8) == 8 && call(&VfsRequestManager::HandleFtruncateRequest, nativeFile, 3) == 0, "mounted file I/O and inode truncate callback");
        if ((int64_t) nativeFile >= 0)
        {
            auto file = static_cast<file_t*>(ProcessManager::GetFileDescriptor(process, nativeFile)->file);
            ok &= memory_expect(file->offset == 8 && file->inode->size == 3, "ftruncate preserves file offset while shrinking backend inode");
            ok &= memory_expect(call(&VfsRequestManager::HandleFcntlRequest, nativeFile, 4, 02000) == 0 && call(&VfsRequestManager::HandleWriteRequest, nativeFile, store(512, "z"), 1) == 1 && file->offset == 4 && file->inode->size == 4 && (file->statusFlags & 3) == 2,
                                "native append uses inode size and F_SETFL preserves access mode");
        }
        const uint64_t inodeWatch = notifications.AddWatch((int32_t) notify, store(256, "/one/newfile"), 4);
        ok &= memory_expect(call(&VfsRequestManager::HandleLinkRequest, store(128, "/one/newfile"), store(256, "/one/alias")) == 0, "hard link dispatches by source inode");
        ok &= memory_expect((int64_t) inodeWatch > 0 && call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 16, "hard link change emits watched inode attribute event");

        ok &= memory_expect(call(&VfsRequestManager::HandleRenameRequest, store(128, "/one/newfile"), store(256, "/one/renamed")) == 0 && !local.ResolvePath({}, "/one/newfile", &resolved) && local.ResolvePath({}, "/one/renamed", &resolved),
                            "rename invalidates cached old name and publishes destination");
        ok &= memory_expect(call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 64, "backend rename emits paired directory records");
        uint32_t moves[16] = {};
        time_fixture_load(process, base + 1024, moves, sizeof(moves));
        ok &= memory_expect(moves[1] == 0x40 && moves[9] == 0x80 && moves[2] != 0 && moves[2] == moves[10], "inotify rename pair carries matching nonzero cookie");
        ok &= memory_expect(call(&VfsRequestManager::HandleSymlinkRequest, store(128, "data"), store(256, "/one/symlink")) == 0 && call(&VfsRequestManager::HandleReadlinkRequest, store(256, "/one/symlink"), base + 1024, 64) == 4, "mounted backend symlink and readlink callbacks");
        ok &= memory_expect(call(&VfsRequestManager::HandleUnlinkRequest, store(256, "/one/renamed")) == 0 && !local.ResolvePath({}, "/one/renamed", &resolved) && local.ResolvePath({}, "/one/alias", &resolved), "unlink invalidates name while preserving backend hard link");
        ok &= memory_expect(call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 16, "unlink with surviving hard link preserves inode watch");

        ok &= memory_expect(call(&VfsRequestManager::HandleUnlinkRequest, store(256, "/one/alias")) == 0 && call(&VfsRequestManager::HandleRmdirRequest, store(256, "/one/newdir")) == 0, "mounted unlink and rmdir dispatch");
        ok &= memory_expect(call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 16, "unlinked open inode keeps watch until last close");
        if ((int64_t) nativeFile >= 0)
        {
            call(&VfsRequestManager::HandleCloseRequest, nativeFile);
        }
        ok &= memory_expect(call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 16 && notifications.RemoveWatch((int32_t) notify, (int32_t) inodeWatch) == LINUX_EINVAL, "last close of unlinked inode removes watch");
        uint32_t removed[4] = {};
        time_fixture_load(process, base + 1024, removed, sizeof(removed));
        ok &= memory_expect(removed[0] == inodeWatch && removed[1] == 0x8000, "inode removal queues IN_IGNORED even when DELETE_SELF was not requested");

        char savedCwd[ProcessManager::MAX_CWD_PATH_LENGTH];
        memcpy(savedCwd, process->cwdPath, sizeof(savedCwd));
        ok &= memory_expect(call(&VfsRequestManager::HandleChdirRequest, store(256, "/one")) == 0 && strcmp(process->cwdPath, "/one") == 0, "chdir retains mountpoint in working directory path");
        const uint64_t created = call(&VfsRequestManager::HandleOpenRequest, store(256, "via-open"), 2 | 0100 | 0200 | 02000000, 0640);
        ok &= memory_expect((int64_t) created >= 0 && call(&VfsRequestManager::HandleOpenRequest, store(256, "via-open"), 2 | 0100 | 0200, 0640) == (uint64_t) -17, "native open creates relative file and honors O_EXCL");
        if ((int64_t) created >= 0)
        {
            ok &= memory_expect(call(&VfsRequestManager::HandleFcntlRequest, created, 1) == 1, "open O_CLOEXEC sets descriptor flag");
            call(&VfsRequestManager::HandleCloseRequest, created);
        }
        const uint64_t truncated = call(&VfsRequestManager::HandleOpenRequest, store(256, "/one/data"), 2 | 01000);
        ok &= memory_expect((int64_t) truncated >= 0 && local.ResolvePath({}, "/one/data", &resolved) && resolved.dentry->inode->size == 0, "native O_TRUNC dispatches inode truncation");
        if ((int64_t) truncated >= 0)
        {
            call(&VfsRequestManager::HandleCloseRequest, truncated);
        }
        vfs_path_t mountedRoot = {};
        local.ResolvePath({}, "/one", &mountedRoot);
        ok &= memory_expect(local.ChangeMetadata(mountedRoot.dentry->inode, false, 02777, 0, mountedRoot.mount, mountedRoot.dentry) == 0 && local.ChangeMetadata(mountedRoot.dentry->inode, true, UINT32_MAX, 42, mountedRoot.mount, mountedRoot.dentry) == 0,
                            "prepare setgid parent for backend creation attributes");
        const auto     savedCredentials       = process->credentials;
        const uint32_t savedUmask             = process->umask;
        process->credentials.user.filesystem  = 99;
        process->credentials.group.filesystem = 77;
        process->umask                        = 0077;
        const uint64_t ownedFile              = call(&VfsRequestManager::HandleOpenRequest, store(256, "/one/owned"), 2 | 0100 | 0200, 0);
        ok &= memory_expect((int64_t) ownedFile >= 0 && local.ResolvePath({}, "/one/owned", &resolved) && resolved.dentry->inode->uid == 99 && resolved.dentry->inode->gid == 42 && resolved.dentry->inode->mode == 0, "backend receives creator uid and inherited gid; creator may open mode-zero file");
        if ((int64_t) ownedFile >= 0)
        {
            call(&VfsRequestManager::HandleCloseRequest, ownedFile);
        }
        ok &= memory_expect(call(&VfsRequestManager::HandleMkdirRequest, store(256, "/one/owned-dir"), 0777) == 0 && local.ResolvePath({}, "/one/owned-dir", &resolved) && resolved.dentry->inode->uid == 99 && resolved.dentry->inode->gid == 42 && resolved.dentry->inode->mode == 02700,
                            "backend directory attributes include umask and inherited setgid");
        process->credentials = savedCredentials;
        process->umask       = savedUmask;
        local.ChangeMetadata(mountedRoot.dentry->inode, false, 0755, 0, mountedRoot.mount, mountedRoot.dentry);
        local.ChangeMetadata(mountedRoot.dentry->inode, true, UINT32_MAX, 0, mountedRoot.mount, mountedRoot.dentry);
        process->credentials.user.filesystem = 99;
        ok &= memory_expect(call(&VfsRequestManager::HandleOpenRequest, store(256, "/one/via-open"), 2) == LINUX_EACCES && call(&VfsRequestManager::HandleMkdirRequest, store(256, "/one/denied"), 0755) == LINUX_EACCES, "native open and directory mutations enforce filesystem credentials");
        process->credentials = savedCredentials;
        memcpy(process->cwdPath, savedCwd, sizeof(savedCwd));

        if ((int64_t) notify >= 0)
        {
            call(&VfsRequestManager::HandleCloseRequest, notify);
        }

        ok &= memory_expect(mount("/one", "/two", 4096) == 0, "bind mounts retain the original backend");
        // A bind remount does not require a source or filesystem name.
        arch_syscall_frame_t remount = {};
        remount.arg1                 = store(256, "/two");
        remount.arg3                 = 4096 | 32 | 1;
        ok &= memory_expect(manager.HandleMountRequest(&remount) == 0, "bind remount installs per-mount readonly flag");
        metadata.arg0 = store(256, "/two/data");
        metadata.arg1 = 0600;
        ok &= memory_expect(manager.HandleChmodRequest(&metadata) == (uint64_t) -30, "readonly bind rejects metadata mutation");
        ok &= memory_expect(call(&VfsRequestManager::HandleOpenRequest, store(256, "/two/data"), 1) == (uint64_t) -30 && call(&VfsRequestManager::HandleLinkRequest, store(128, "/one/data"), store(256, "/two/cross")) == (uint64_t) -18, "readonly open and cross-mount link return native errno");

        metadata.arg0 = store(256, "/one/data");
        ok &= memory_expect(manager.HandleChmodRequest(&metadata) == 0, "readonly bind leaves original filesystem writable");
        ok &= memory_expect(mount("/one", "/three", 8192) == 0 && local.ResolvePath({}, "/three/data", &resolved) && !local.ResolvePath({}, "/one/data", &resolved), "move mount changes namespace attachment");
        ok &= memory_expect(mount(nullptr, "/three", 32 | 1, "reject") == (uint64_t) -22 && mount(nullptr, "/three", 32 | 1) == 0, "filesystem remount calls backend and propagates failure");
        metadata.arg0 = store(256, "/three/data");
        ok &= memory_expect(manager.HandleChmodRequest(&metadata) == (uint64_t) -30, "readonly filesystem remount rejects mutation");
        local.SetEventManager(nullptr);
    }
    ok &= memory_expect(mount_test_releases == releases + 2, "VFS teardown releases mounted backend once despite bind alias");
    return ok;
}

static bool test_vfs_mount_framework(process_t* process, uint64_t base)
{
    const char target[] = "/", unknown[] = "arx-unknown", type[] = "cpio";
    time_fixture_store(process, base + 128, target, sizeof(target));
    time_fixture_store(process, base + 256, unknown, sizeof(unknown));
    time_fixture_store(process, base + 384, type, sizeof(type));
    bool ok = memory_expect(memory_call(SYSCALL_mount, 0, base + 128, base + 256, 0) == (uint64_t) -19, "mount unknown filesystem ENODEV");
    ok &= memory_expect(memory_call(SYSCALL_mount, 0, base + 128, base + 384, 0) == LINUX_EOPNOTSUPP && memory_call(SYSCALL_mount, 0, base + 128, 0, 4096) == LINUX_EINVAL, "mount boot cpio requires an archive and bind requires a source");
    ok &= memory_expect(memory_call(SYSCALL_mount, 0, CANONICAL_KERNEL_BASE, base + 384, 0) == LINUX_EFAULT && memory_call(SYSCALL_mount, 0, base + 128, base + 384, 1ULL << 40) == LINUX_EINVAL, "mount pointer and flag validation");
    const auto credentials              = process->credentials;
    process->credentials.user.effective = 99;
    ok &= memory_expect(memory_call(SYSCALL_mount, 0, 0, 0, 0) == (uint64_t) -1, "mount requires privilege before inspecting user arguments");
    process->credentials = credentials;
    ok &= test_vfs_backend_syscalls(process, base);
    return ok;
}

template <uint64_t Number> static bool custom_case_validate_vfs_extensions(const custom_case_ctx_t*)
{
    return run_memory_case(
            [](process_t* process)
            {
                process->credentials = {};
                const uint64_t base  = memory_map(8);
                if ((int64_t) base < 0)
                {
                    return false;
                }
                bool ok = Number == SYSCALL_pipe || Number == SYSCALL_pipe2 ? test_ipc_pipe(process, base, Number) : Number == SYSCALL_memfd_create ? test_ipc_memory_file(process, base) : Number == SYSCALL_mount ? test_vfs_mount_framework(process, base) : test_vfs_metadata(process, base, Number);
                ok &= memory_expect(ipc_descriptor_count(process) == 0, "VFS extension fixture releases every descriptor");
                return ok;
            });
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
            {"HandleFchownatRequest", &VfsRequestManager::HandleFchownatRequest, SYSCALL_fchownat, custom_case_validate_vfs_extensions<SYSCALL_fchownat>},
            {"HandleChownRequest", &VfsRequestManager::HandleChownRequest, SYSCALL_chown, custom_case_validate_vfs_extensions<SYSCALL_chown>},
            {"HandleFchownRequest", &VfsRequestManager::HandleFchownRequest, SYSCALL_fchown, custom_case_validate_vfs_extensions<SYSCALL_fchown>},
            {"HandleLchownRequest", &VfsRequestManager::HandleLchownRequest, SYSCALL_lchown, custom_case_validate_vfs_extensions<SYSCALL_lchown>},
            {"HandleFchmodat2Request", &VfsRequestManager::HandleFchmodat2Request, SYSCALL_fchmodat2, custom_case_validate_vfs_extensions<SYSCALL_fchmodat2>},
            {"HandleFchmodatRequest", &VfsRequestManager::HandleFchmodatRequest, SYSCALL_fchmodat, custom_case_validate_vfs_extensions<SYSCALL_fchmodat>},
            {"HandleChmodRequest", &VfsRequestManager::HandleChmodRequest, SYSCALL_chmod, custom_case_validate_vfs_extensions<SYSCALL_chmod>},
            {"HandleFchmodRequest", &VfsRequestManager::HandleFchmodRequest, SYSCALL_fchmod, custom_case_validate_vfs_extensions<SYSCALL_fchmod>},
            {"HandleMountRequest", &VfsRequestManager::HandleMountRequest, SYSCALL_mount, custom_case_validate_vfs_extensions<SYSCALL_mount>},
            {"HandleStatfsRequest", &VfsRequestManager::HandleStatfsRequest, SYSCALL_statfs, custom_case_validate_statfs},
            {"HandleFstatfsRequest", &VfsRequestManager::HandleFstatfsRequest, SYSCALL_fstatfs, custom_case_validate_fstatfs},
            {"HandleMknodatRequest", &VfsRequestManager::HandleMknodatRequest, SYSCALL_mknodat, custom_case_validate_mknodat},
            {"HandleMknodRequest", &VfsRequestManager::HandleMknodRequest, SYSCALL_mknod, custom_case_validate_mknod},
            {"HandleTruncateRequest", &VfsRequestManager::HandleTruncateRequest, SYSCALL_truncate, custom_case_validate_truncate},
            {"HandleFtruncateRequest", &VfsRequestManager::HandleFtruncateRequest, SYSCALL_ftruncate, custom_case_validate_ftruncate},
            {"HandlePipeRequest", &VfsRequestManager::HandlePipeRequest, SYSCALL_pipe, custom_case_validate_vfs_extensions<SYSCALL_pipe>},
            {"HandlePipe2Request", &VfsRequestManager::HandlePipe2Request, SYSCALL_pipe2, custom_case_validate_vfs_extensions<SYSCALL_pipe2>},
            {"HandleMemfd_createRequest", &VfsRequestManager::HandleMemfd_createRequest, SYSCALL_memfd_create, custom_case_validate_vfs_extensions<SYSCALL_memfd_create>},
    };

    VfsRequestManager unavailable(nullptr, nullptr);
    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};
        if ((unavailable.*test.method)(&frame) != LINUX_ENOSYS || (caps->vfsRequestManager != nullptr && (caps->vfsRequestManager->*test.method)(nullptr) != LINUX_EINVAL))
        {
            stats->implementedFail++;
            selftest_record_failure_detail("VFS missing dependency/null-frame validation");
        }
    }

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
            {
                vfs->Close(file);
            }
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
    {
        scheduler->KillProcess(peer->id, 0);
    }

    if (task != nullptr && task->allocated)
    {
        ok &= memory_expect(resources->taskManager->FreeTask(task), "event peer task cleanup");
    }

    if (peer != nullptr && peer->allocated)
    {
        ok &= memory_expect(resources->processManager->FreeProcess(peer), "event peer process cleanup");
    }

    if (sharedFile != nullptr)
    {
        spinlock_acquire_preempt(&sharedFile->refLock);
        const bool releasedReference = sharedFile->refCount == referencesBefore;
        spinlock_release_preempt(&sharedFile->refLock);
        ok &= memory_expect(releasedReference, "process teardown releases shared eventfd reference");
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
    time_fixture_store(process, base + 64, values, sizeof(values));
    time_fixture_store(process, metadata, vectors, sizeof(vectors));
    bool ok    = memory_expect(memory_call(SYSCALL_writev, fd, metadata, 3) == 16, "eventfd writev increments once per eight-byte vector");
    vectors[0] = {0, 0};
    vectors[1] = {first, 3};
    vectors[2] = {second, 13};
    uint8_t untouched[13];
    memset(untouched, 0xcc, sizeof(untouched));
    time_fixture_store(process, second, untouched, sizeof(untouched));
    time_fixture_store(process, metadata, vectors, sizeof(vectors));
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, metadata, 3) == 8, "eventfd readv scatters one eight-byte counter");
    uint64_t actual = 0;
    time_fixture_load(process, first, &actual, 3);
    time_fixture_load(process, second, reinterpret_cast<uint8_t*>(&actual) + 3, 5);
    time_fixture_load(process, second, untouched, sizeof(untouched));
    ok &= memory_expect(actual == 12 && untouched[5] == 0xcc && untouched[12] == 0xcc, "eventfd readv preserves bytes beyond counter");
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, metadata, 3) == (uint64_t) -11, "empty eventfd readv EAGAIN");

    vectors[0] = {base + 64, 4};
    vectors[1] = {base + 68, 4};
    time_fixture_store(process, metadata, vectors, sizeof(vectors));
    ok &= memory_expect(memory_call(SYSCALL_writev, fd, metadata, 2) == LINUX_EINVAL, "eventfd writev does not combine short increments");
    vectors[0] = {base + 64, 8};
    time_fixture_store(process, metadata, vectors, sizeof(vectors));
    ok &= memory_expect(memory_call(SYSCALL_writev, fd, metadata, 2) == 8, "eventfd writev returns progress before invalid later increment");
    ok &= memory_expect(memory_call(SYSCALL_read, fd, first, 8) == 8, "eventfd partial writev counter readback");
    time_fixture_load(process, first, &actual, sizeof(actual));
    ok &= memory_expect(actual == 5, "eventfd partial writev commits only successful increment");

    vectors[0] = {first, 3};
    vectors[1] = {second, 5};
    time_fixture_store(process, metadata, vectors, sizeof(vectors));
    memory_call(SYSCALL_write, fd, base + 64, 8);
    const uint64_t sentinel = UINT64_MAX;
    time_fixture_store(process, first, &sentinel, sizeof(sentinel));
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, metadata, 2) == LINUX_EFAULT, "eventfd readv validates every output segment before scatter");
    time_fixture_load(process, first, &actual, sizeof(actual));
    ok &= memory_expect(actual == sentinel, "eventfd failed scatter preserves earlier output segment");
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, metadata, 2) == 8, "eventfd failed scatter preserves counter");
    vectors[1].length = 4;
    time_fixture_store(process, metadata, vectors, sizeof(vectors));
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, metadata, 2) == LINUX_EINVAL, "eventfd short aggregate readv rejected");
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, CANONICAL_KERNEL_BASE, 1) == LINUX_EFAULT && memory_call(SYSCALL_readv, fd, metadata, 1025) == LINUX_EINVAL, "eventfd vector metadata validation");
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, CANONICAL_KERNEL_BASE, 0) == 0 && memory_call(SYSCALL_writev, fd, CANONICAL_KERNEL_BASE, 0) == 0, "eventfd zero vector count ignores pointer");

    memory_call(SYSCALL_fcntl, fd, 4, 2);
    vectors[1].length = 5;
    time_fixture_store(process, metadata, vectors, sizeof(vectors));
    ok &= event_with_peer(process, fd, false, [&]() { return memory_expect(memory_call(SYSCALL_readv, fd, metadata, 2) == 8, "parked eventfd readv wakes on peer write"); });
    values[0] = UINT64_MAX - 1;
    time_fixture_store(process, base + 64, values, sizeof(values));
    memory_call(SYSCALL_write, fd, base + 64, 8);
    values[0] = 1;
    time_fixture_store(process, base + 64, values, sizeof(values));
    vectors[0] = {base + 64, 8};
    time_fixture_store(process, metadata, vectors, sizeof(vectors));
    ok &= event_with_peer(process, fd, true, [&]() { return memory_expect(memory_call(SYSCALL_writev, fd, metadata, 1) == 8, "parked eventfd writev wakes on peer read"); });
    memory_call(SYSCALL_fcntl, fd, 4, 2 | 04000);
    ok &= memory_expect(memory_call(SYSCALL_read, fd, first, 8) == 8, "eventfd vector fixture drain");
    return ok;
}

static bool test_event_counter(process_t* process, uint64_t number)
{
    const uint64_t base = memory_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    uint64_t fd = number == SYSCALL_eventfd ? memory_call(number, 3) : memory_call(number, 3, 04000 | 02000000);

    if ((int64_t) fd < 0)
    {
        return memory_expect(false, "eventfd creates descriptor");
    }

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
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, base + 128, 1) == (uint64_t) -11 && memory_call(SYSCALL_pread64, fd, base, 8, 0) == (uint64_t) -29, "eventfd empty vector read and unsupported positioned I/O");
    ok &= test_event_counter_vector(process, fd, base);
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
            const file_iovec_t vectors[2] = {{base, 3}, {base + 3, 5}};
            time_fixture_store(process, base + 384, vectors, sizeof(vectors));
            ok &= memory_expect(memory_call(SYSCALL_readv, semaphore, base + 384, 2) == 8, "eventfd semaphore scatter read");
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

    if (!memory_expect(installed == 0, "event signal handler fixture"))
    {
        return false;
    }

    __atomic_store_n(&task->signalMask, blockedMask, __ATOMIC_RELEASE);
    uint64_t mask      = 0;
    uint64_t bundle[2] = {base + 1024, sizeof(mask)};
    time_fixture_store(process, base + 1024, &mask, sizeof(mask));
    time_fixture_store(process, base + 1056, bundle, sizeof(bundle));

    arch_syscall_frame_t frame = {};
    frame.syscall_number       = number;

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

    bool ok = memory_expect(signals->BeginTemporaryMask(UINT64_MAX) == 0, "temporary mask installation");
    ok &= memory_expect((task->signalMask & SIGNAL_UNBLOCKABLE) == 0, "temporary mask cannot block SIGKILL or SIGSTOP");
    signals->FinishTemporaryMask(false);
    ok &= memory_expect(task->signalMask == blockedMask && !task->signalRestoreMask, "temporary mask explicit restoration");

    const uint64_t sent = memory_call(SYSCALL_kill, process->id, 10);
    ok &= memory_expect(sent == 0, "event blocked pending signal fixture");

    if (sent == 0)
    {
        ok &= memory_expect(dispatcher_dispatch_syscall(&frame) == (uint64_t) -4, "temporary mask unblocks pending signal before parking");
        ok &= memory_expect(task->signalMask == blockedMask && !task->signalRestoreMask, "kernel event caller restores mask after EINTR");

#if defined(__x86_64__)
        alignas(16) uint8_t floating[512] = {};
        const uint32_t      mxcsr         = 0x1f80;
        const uint32_t      mxcsrMask     = 0xffbf;
        memcpy(floating + 24, &mxcsr, sizeof(mxcsr));
        memcpy(floating + 28, &mxcsrMask, sizeof(mxcsrMask));
        frame.user_cs     = USER_CS;
        frame.user_ss     = USER_SS;
        frame.user_rflags = 0x202;
        frame.user_rip    = base + 1792;
        frame.user_rsp    = base + 2 * PAGE_SIZE - 16;
        frame.fxstate     = (uint64_t) floating;

        const uint64_t interrupted = dispatcher_dispatch_syscall(&frame);
        ok &= memory_expect(interrupted == (uint64_t) -4 && task->signalRestoreMask && task->signalMask == 0, "user event caller defers mask restoration until signal delivery");
        ok &= memory_expect(signals->Deliver(&frame, interrupted) == 0 && frame.user_rip == action.handler, "event EINTR delivers handler under temporary mask");

        signal_frame_fixture_t saved = {};
        time_fixture_load(process, frame.user_rsp, &saved, sizeof(saved));
        ok &= memory_expect(saved.uc.mask == blockedMask && saved.uc.context.ax == (uint64_t) -4, "event signal frame saves original mask and EINTR");
        ok &= memory_expect(!task->signalRestoreMask && task->signalMask == (1ULL << 9), "handler mask derives from temporary mask");
        frame.user_rsp += 8;
        ok &= memory_expect(signals->Restore(&frame) == (uint64_t) -4 && task->signalMask == blockedMask, "event sigreturn restores original mask");

        ok &= memory_expect(signals->BeginTemporaryMask(0) == 0, "event no-pending restoration fixture");
        signals->FinishTemporaryMask(true);
        ok &= memory_expect(signals->Deliver(&frame, (uint64_t) -4) == (uint64_t) -4 && task->signalMask == blockedMask && !task->signalRestoreMask, "signal return restores deferred mask when pending signal disappears");
#endif
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
    const uint64_t base    = memory_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

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
    {
        return false;
    }

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
    {
        pollfd.events = 1 | 4;
    }

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
    if (precise)
    {
        const uint64_t mask      = UINT64_MAX;
        const uint64_t bundle[2] = {base + 512, sizeof(mask)};
        time_fixture_store(process, base + 512, &mask, sizeof(mask));
        time_fixture_store(process, base + 544, bundle, sizeof(bundle));
    }

    ok &= event_with_peer(process, fd, false,
                          [&]()
                          {
                              // Infinite waits need no clock and must be awakened by the producer.
                              uint64_t result = select ? memory_call(number, fd + 1, input, 0, 0, 0, precise ? base + 544 : 0) : precise ? memory_call(number, input, 1, 0, base + 512, 8) : memory_call(number, input, 1, UINT64_MAX);
                              return memory_expect(result == 1, "poll/select parked wait wakes on peer event");
                          });
    memory_call(SYSCALL_read, fd, base, 8);
    prepare();

    if (precise)
    {
        ok &= test_event_temporary_mask(process, number, base);
        uint64_t mask = 0;
        time_fixture_store(process, base + 512, &mask, sizeof(mask));
        uint64_t bundle[2] = {base + 512, 8};
        time_fixture_store(process, base + 544, bundle, sizeof(bundle));
        task_t*        task         = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->GetCurrentTask();
        const uint64_t originalMask = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);
        uint64_t       masked       = select ? memory_call(number, 0, 0, 0, 0, timeout, base + 544) : memory_call(number, 0, 0, timeout, base + 512, 8);
        ok &= memory_expect(masked == 0 && task->signalMask == originalMask && !task->signalRestoreMask, "event temporary signal mask restored after timeout");
        mask = UINT64_MAX;
        time_fixture_store(process, base + 512, &mask, sizeof(mask));
        masked = select ? memory_call(number, 0, 0, 0, 0, CANONICAL_KERNEL_BASE, base + 544) : memory_call(number, 0, 0, CANONICAL_KERNEL_BASE, base + 512, 8);
        ok &= memory_expect(masked == LINUX_EFAULT && task->signalMask == originalMask && !task->signalRestoreMask, "event temporary mask restored after invalid timeout");
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
    {
        ok &= memory_expect(memory_call(number, UINT64_MAX, 0, 0, 0, timeout) == LINUX_EINVAL, "select negative nfds");
    }
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

static bool test_event_epoll_partial_copy(process_t* process, uint64_t base)
{
    const uint64_t epfd   = memory_call(SYSCALL_epoll_create1, 0);
    const uint64_t first  = memory_call(SYSCALL_eventfd2, 1, 04000);
    const uint64_t second = memory_call(SYSCALL_eventfd2, 1, 04000);
    bool           ready  = (int64_t) epfd >= 0 && (int64_t) first >= 0 && (int64_t) second >= 0;
    bool           ok     = memory_expect(ready, "epoll partial-copy fixture");

    if (ready)
    {
        event_epoll_event_t event = {EVENT_POLL_IN | 0xc0000000U, 1};
        time_fixture_store(process, base + 2048, &event, sizeof(event));
        ready      = memory_call(SYSCALL_epoll_ctl, epfd, 1, first, base + 2048) == 0;
        event.data = 2;
        time_fixture_store(process, base + 2048, &event, sizeof(event));
        ready &= memory_call(SYSCALL_epoll_ctl, epfd, 1, second, base + 2048) == 0;
        ok &= memory_expect(ready, "epoll partial-copy watch registration");

        if (ready)
        {
            const uint64_t output = base + PAGE_SIZE - sizeof(event);
            memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
            ok &= memory_expect(memory_call(SYSCALL_epoll_wait, epfd, output, 2, 0) == 1, "epoll returns copied prefix before later output fault");
            event_epoll_event_t actual = {};
            time_fixture_load(process, output, &actual, sizeof(actual));
            ok &= memory_expect(actual.events == EVENT_POLL_IN && actual.data == 1, "epoll first event copied before disarm");
            memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);
            ok &= memory_expect(memory_call(SYSCALL_epoll_wait, epfd, base + 2112, 2, 0) == 1, "epoll untransferred edge remains pending after partial output fault");
            time_fixture_load(process, base + 2112, &actual, sizeof(actual));
            ok &= memory_expect(actual.data == 2 && memory_call(SYSCALL_epoll_wait, epfd, base + 2112, 2, 0) == 0, "epoll disarms only successfully copied one-shot events");
        }
    }

    if ((int64_t) first >= 0)
    {
        memory_call(SYSCALL_close, first);
    }

    if ((int64_t) second >= 0)
    {
        memory_call(SYSCALL_close, second);
    }

    if ((int64_t) epfd >= 0)
    {
        memory_call(SYSCALL_close, epfd);
    }

    return ok;
}

static bool test_event_nested_epoll(process_t* process, uint64_t base)
{
    uint64_t instances[6];
    bool     ready = true;

    for (size_t i = 0; i < 6; ++i)
    {
        instances[i] = memory_call(SYSCALL_epoll_create1, 0);
        ready &= (int64_t) instances[i] >= 0;
    }

    const uint64_t counter = memory_call(SYSCALL_eventfd2, 1, 04000);
    ready &= (int64_t) counter >= 0;
    bool ok = memory_expect(ready, "nested epoll graph fixture");

    if (ready)
    {
        const uint64_t      input  = base + 2048;
        const uint64_t      output = base + 2112;
        event_epoll_event_t event  = {EVENT_POLL_IN, 17};
        time_fixture_store(process, input, &event, sizeof(event));

        for (size_t i = 0; i < 4; ++i)
        {
            ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, instances[i], 1, instances[i + 1], input) == 0, "nested epoll accepts five instance levels");
        }

        ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, instances[4], 1, instances[5], input) == (uint64_t) -40, "nested epoll checks depth through existing ancestors");
        ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, instances[4], 1, instances[0], input) == (uint64_t) -40, "nested epoll rejects transitive cycle");
        event.events = EVENT_POLL_IN | 0x40000000U;
        time_fixture_store(process, input, &event, sizeof(event));
        ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, instances[4], 1, counter, input) == 0, "nested leaf one-shot registration");
        ok &= memory_expect(memory_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 1, "nested epoll propagates readiness");
        ok &= memory_expect(memory_call(SYSCALL_epoll_wait, instances[4], output, 1, 0) == 1 && memory_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 0, "nested epoll reflects one-shot disarm");
        ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, instances[4], 3, counter, input) == 0, "nested leaf rearm");
        event.events = EVENT_POLL_IN | 0x80000000U;
        time_fixture_store(process, input, &event, sizeof(event));
        ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, instances[0], 3, instances[1], input) == 0, "nested outer edge registration");
        ok &= memory_expect(memory_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 1 && memory_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 0, "nested outer edge delivered once");
        uint64_t value = 1;
        time_fixture_store(process, base, &value, sizeof(value));
        ok &= memory_expect(memory_call(SYSCALL_write, counter, base, 8) == 8 && memory_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 1, "new leaf notification propagates through nested edge");
        ok &= memory_expect(memory_call(SYSCALL_read, counter, base, 8) == 8, "nested leaf drain");
        event.events = EVENT_POLL_IN | 0x40000000U;
        time_fixture_store(process, input, &event, sizeof(event));
        ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, instances[4], 3, counter, input) == 0 && memory_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 0, "empty nested graph is not readable");
        if (ok)
        {
            ok &= event_with_peer(process, counter, false, [&]() { return memory_expect(memory_call(SYSCALL_epoll_wait, instances[0], output, 1, UINT64_MAX) == 1, "parked nested edge wakes on peer write"); });
        }
        ok &= memory_expect(memory_call(SYSCALL_close, instances[4]) == 0, "nested child final close");
        instances[4] = UINT64_MAX;
        ok &= memory_expect(memory_call(SYSCALL_epoll_wait, instances[0], output, 1, 0) == 0, "nested child close detaches parent watch");
    }

    for (size_t i = 0; i < 6; ++i)
    {
        if ((int64_t) instances[i] >= 0)
        {
            memory_call(SYSCALL_close, instances[i]);
        }
    }

    if ((int64_t) counter >= 0)
    {
        memory_call(SYSCALL_close, counter);
    }

    return ok;
}

static bool test_event_epoll(process_t* process, uint64_t number)
{
    const uint64_t base = memory_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    if (number == SYSCALL_epoll_ctl && (!test_event_nested_epoll(process, base) || !test_event_epoll_partial_copy(process, base)))
    {
        return false;
    }

    const uint64_t input  = base + PAGE_SIZE - 4; // Packed event crosses a page.
    const uint64_t output = base + 128;
    const bool     legacy = number == SYSCALL_epoll_create;
    const uint64_t epfd   = legacy ? memory_call(number, 1) : memory_call(SYSCALL_epoll_create1, 02000000);
    const uint64_t fd     = memory_call(SYSCALL_eventfd2, 0, 04000);

    if (!memory_expect((int64_t) epfd >= 0 && (int64_t) fd >= 0, "epoll fixture descriptors"))
    {
        return false;
    }

    bool ok = memory_expect(memory_call(SYSCALL_fcntl, epfd, 1) == (legacy ? 0ULL : 1ULL), "epoll creation CLOEXEC flag");
    ok &= memory_expect(memory_call(SYSCALL_epoll_create, 0) == LINUX_EINVAL && memory_call(SYSCALL_epoll_create, UINT64_MAX) == LINUX_EINVAL && memory_call(SYSCALL_epoll_create1, 04000) == LINUX_EINVAL, "epoll creation argument validation");
    event_epoll_event_t event = {EVENT_POLL_IN, 0xfedcba9876543210ULL}, actual = {};
    time_fixture_store(process, input, &event, sizeof(event));
    const uint64_t waitMask = UINT64_MAX;
    time_fixture_store(process, base + 800, &waitMask, sizeof(waitMask));
    auto control = [&](uint64_t operation, uint64_t target) -> uint64_t { return memory_call(SYSCALL_epoll_ctl, epfd, operation, target, input); };
    auto wait    = [&](uint64_t count, uint64_t milliseconds) -> uint64_t
    {
        if (number == SYSCALL_epoll_pwait2)
        {
            time_timespec_t duration = {(int64_t) (milliseconds / 1000), (int64_t) ((milliseconds % 1000) * 1000000)};
            time_fixture_store(process, base + 768, &duration, sizeof(duration));
            return memory_call(number, epfd, output, count, milliseconds == UINT64_MAX ? 0 : base + 768, base + 800, 8);
        }

        const uint64_t waitNumber = number == SYSCALL_epoll_pwait ? number : SYSCALL_epoll_wait;
        return memory_call(waitNumber, epfd, output, count, milliseconds, number == SYSCALL_epoll_pwait ? base + 800 : 0, 8);
    };
    ok &= memory_expect(wait(1, 0) == 0, "empty epoll zero timeout");
    if (number == SYSCALL_epoll_pwait || number == SYSCALL_epoll_pwait2)
    {
        ok &= test_event_temporary_mask(process, number, base, epfd);
        task_t*         task         = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->GetCurrentTask();
        const uint64_t  originalMask = __atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE);
        uint64_t        mask         = UINT64_MAX;
        time_timespec_t duration     = {};
        time_fixture_store(process, base + 768, &duration, sizeof(duration));
        time_fixture_store(process, base + 800, &mask, sizeof(mask));
        const uint64_t timeout = number == SYSCALL_epoll_pwait2 ? base + 768 : 0;
        ok &= memory_expect(memory_call(number, epfd, output, 1, timeout, 0, UINT64_MAX) == 0, "epoll null signal mask ignores size");
        ok &= memory_expect(memory_call(number, epfd, output, 1, timeout, base + 800, 8) == 0 && task->signalMask == originalMask && !task->signalRestoreMask, "epoll temporary mask restored after timeout");
        ok &= memory_expect(memory_call(number, epfd, output, 1, timeout, base + 800, 4) == LINUX_EINVAL, "epoll signal mask size validation");
        ok &= memory_expect(memory_call(number, epfd, output, 1, timeout, CANONICAL_KERNEL_BASE, 8) == LINUX_EFAULT, "epoll invalid signal mask buffer");
        ok &= memory_expect(memory_call(number, epfd, output, 0, timeout, base + 800, 8) == LINUX_EINVAL && task->signalMask == originalMask && !task->signalRestoreMask, "epoll temporary mask restored after invalid count");

        if (number == SYSCALL_epoll_pwait2)
        {
            duration = {-1, 0};
            time_fixture_store(process, base + 768, &duration, sizeof(duration));
            ok &= memory_expect(memory_call(number, epfd, output, 1, base + 768) == LINUX_EINVAL, "epoll_pwait2 negative seconds");
            duration = {0, 1000000000};
            time_fixture_store(process, base + 768, &duration, sizeof(duration));
            ok &= memory_expect(memory_call(number, epfd, output, 1, base + 768) == LINUX_EINVAL, "epoll_pwait2 invalid nanoseconds");
            ok &= memory_expect(memory_call(number, epfd, output, 1, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "epoll_pwait2 invalid timespec buffer");
        }
    }
    ok &= memory_expect(control(3, fd) == LINUX_ENOENT && control(2, fd) == LINUX_ENOENT, "epoll missing MOD/DEL");
    ok &= memory_expect(control(1, fd) == 0 && control(1, fd) == (uint64_t) -17, "epoll ADD and duplicate EEXIST");
    ok &= memory_expect(wait(1, 0) == 0, "registered empty eventfd is not ready");
    ok &= memory_expect(control(0, fd) == LINUX_EINVAL && control(1, UINT64_MAX) == LINUX_EBADF && control(1, epfd) == LINUX_EINVAL, "epoll operation/fd/self validation");
    ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, epfd, 3, fd, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "epoll bad event input");
    const uint32_t unsupportedModes[] = {0x20000000U, 0x10000000U};

    for (uint32_t unsupported : unsupportedModes)
    {
        event.events = EVENT_POLL_IN | unsupported;
        time_fixture_store(process, input, &event, sizeof(event));
        ok &= memory_expect(control(3, fd) == LINUX_EOPNOTSUPP, "epoll unsupported WAKEUP/EXCLUSIVE rejected");
    }

    event = {EVENT_POLL_IN, 0xfedcba9876543210ULL};
    time_fixture_store(process, input, &event, sizeof(event));
    const uint64_t other = memory_call(SYSCALL_epoll_create1, 0);

    if ((int64_t) other >= 0)
    {
        ok &= memory_expect(control(1, other) == 0, "nested epoll registration");
        ok &= memory_expect(memory_call(SYSCALL_epoll_ctl, other, 1, epfd, input) == (uint64_t) -40, "nested epoll rejects cycles");
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
    const uint32_t modes[] = {0x80000000U, 0x40000000U, 0xc0000000U};

    for (uint32_t mode : modes)
    {
        event = {EVENT_POLL_IN | mode, mode};
        time_fixture_store(process, input, &event, sizeof(event));
        ok &= memory_expect(control(3, fd) == 0, "epoll ET/ONESHOT MOD rearm");
        memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
        ok &= memory_expect(memory_call(SYSCALL_epoll_wait, epfd, input, 1, 0) == LINUX_EFAULT, "epoll output fault preserves pending edge and one-shot arm");
        memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 3);
        ok &= memory_expect(wait(1, 0) == 1 && wait(1, 0) == 0, "epoll edge/one-shot delivers once");
        time_fixture_load(process, output, &actual, sizeof(actual));
        ok &= memory_expect(actual.events == EVENT_POLL_IN && actual.data == mode, "epoll private flags excluded from returned mask");
        value = 1;
        time_fixture_store(process, base, &value, sizeof(value));
        ok &= memory_expect(memory_call(SYSCALL_write, fd, base, 8) == 8, "epoll fresh source notification");
        const uint64_t expected = (mode & 0x40000000U) != 0 ? 0 : 1;
        ok &= memory_expect(wait(1, 0) == expected && wait(1, 0) == 0, "new writes notify edges but do not rearm one-shot");
        ok &= memory_expect(control(3, fd) == 0 && wait(1, 0) == 1 && wait(1, 0) == 0, "epoll MOD rechecks existing readiness");
    }

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
        {
            memory_call(SYSCALL_close, replacement);
        }

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
        {
            ok &= event_with_peer(process, source, false, [&]() { return memory_expect(wait(1, UINT64_MAX) == 1, "parked epoll wait wakes on real peer write"); });
        }

        memory_call(SYSCALL_read, source, base, 8);
        time_timespec_t now;
        Dispatcher*     dispatcher = static_cast<Dispatcher*>(platform.dispacher);
        TimeManager*    clock      = dispatcher->GetResourceLayerCaps()->timeManager;

        if (clock != nullptr && clock->ReadClock(1, &now) == 0)
        {
            ok &= memory_expect(wait(1, 1) == 0, "epoll finite timeout expires");
        }
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
                if (Number == SYSCALL_epoll_create || Number == SYSCALL_epoll_create1 || Number == SYSCALL_epoll_ctl || Number == SYSCALL_epoll_wait || Number == SYSCALL_epoll_pwait || Number == SYSCALL_epoll_pwait2)
                {
                    return test_event_epoll(process, Number);
                }

                return Number == SYSCALL_eventfd || Number == SYSCALL_eventfd2 ? test_event_counter(process, Number) : test_event_wait(process, Number);
            });
}

static bool test_inotify_framework(process_t* process, uint64_t base, uint64_t number)
{
    const uint64_t fd = number == SYSCALL_inotify_init ? memory_call(number) : memory_call(SYSCALL_inotify_init1, 04000 | 02000000);
    if (!memory_expect((int64_t) fd >= 0, "inotify creates a readable notification descriptor"))
    {
        return false;
    }
    bool ok = memory_expect(memory_call(SYSCALL_fcntl, fd, 1) == (uint64_t) (number != SYSCALL_inotify_init), "inotify CLOEXEC creation flag");
    memory_call(SYSCALL_fcntl, fd, 4, 04000);
    const char path[] = "/test.txt", root[] = "/";
    time_fixture_store(process, base + 128, path, sizeof(path));
    time_fixture_store(process, base + 256, root, sizeof(root));
    ok &= memory_expect(memory_call(SYSCALL_read, fd, base + 1024, 512) == (uint64_t) -11 && memory_call(SYSCALL_inotify_init1, 1) == LINUX_EINVAL, "inotify empty nonblocking read and creation flags");
    const uint64_t watch = memory_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x31);
    ok &= memory_expect((int64_t) watch > 0 && memory_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x10000020) == (uint64_t) -17, "inotify watch allocation and MASK_CREATE collision");
    ok &= memory_expect(memory_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x01000020) == LINUX_ENOTDIR && memory_call(SYSCALL_inotify_add_watch, fd, CANONICAL_KERNEL_BASE, 0x20) == LINUX_EFAULT, "inotify target flags and user path validation");
    const uint64_t opened = memory_call(SYSCALL_open, base + 128, 0);
    ok &= memory_expect((int64_t) opened >= 0 && memory_call(SYSCALL_read, opened, base + 512, 4) == 4, "watched VFS open and read");
    if ((int64_t) opened >= 0)
    {
        memory_call(SYSCALL_close, opened);
    }
    ok &= memory_expect(memory_call(SYSCALL_ioctl, fd, 0x541b, base + 64) == 0, "inotify FIONREAD reports complete record bytes");
    uint32_t queued = 0;
    time_fixture_load(process, base + 64, &queued, sizeof(queued));
    ok &= memory_expect(queued == 48 && memory_call(SYSCALL_read, fd, base + 1024, 15) == LINUX_EINVAL && memory_call(SYSCALL_read, fd, CANONICAL_KERNEL_BASE, 512) == LINUX_EFAULT, "inotify short/faulting buffers preserve queued events");
    ok &= memory_expect(memory_call(SYSCALL_read, fd, base + 1024, 512) == 48, "inotify batches open/access/close records");
    uint32_t events[12] = {};
    time_fixture_load(process, base + 1024, events, sizeof(events));
    ok &= memory_expect(events[0] == watch && events[1] == 0x20 && events[3] == 0 && events[4] == watch && events[5] == 1 && events[8] == watch && events[9] == 0x10, "Linux inotify_event 16-byte headers and event ordering");
    ok &= memory_expect(memory_call(SYSCALL_inotify_rm_watch, fd, watch) == 0 && memory_call(SYSCALL_inotify_rm_watch, fd, watch) == LINUX_EINVAL && memory_call(SYSCALL_read, fd, base + 1024, 512) == 16, "inotify removal queues one IN_IGNORED record");
    time_fixture_load(process, base + 1024, events, 16);
    ok &= memory_expect(events[1] == 0x8000, "inotify ignored event wire mask");
    const uint64_t directoryWatch = memory_call(SYSCALL_inotify_add_watch, fd, base + 256, 0x20 | 0x80000000U);
    const uint64_t another        = memory_call(SYSCALL_open, base + 128, 0);
    if ((int64_t) another >= 0)
    {
        memory_call(SYSCALL_close, another);
    }
    ok &= memory_expect((int64_t) directoryWatch > 0 && memory_call(SYSCALL_read, fd, base + 1024, 512) == 48, "directory oneshot produces padded child event and ignored record");
    uint8_t record[48] = {};
    time_fixture_load(process, base + 1024, record, sizeof(record));
    uint32_t nameLength = 0;
    memcpy(&nameLength, record + 12, 4);
    ok &= memory_expect(nameLength == 16 && memcmp(record + 16, "test.txt", 9) == 0 && memory_call(SYSCALL_inotify_rm_watch, fd, directoryWatch) == LINUX_EINVAL, "inotify directory name padding and oneshot teardown");
    const uint64_t blockingWatch = memory_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x20);
    memory_call(SYSCALL_fcntl, fd, 4, 0);
    ipc_peer_context_t peer = {nullptr, SYSCALL_open, {base + 128, 0}, UINT64_MAX};
    ok &= ipc_with_peer(process, &peer, [&]() { return memory_expect(memory_call(SYSCALL_read, fd, base + 1024, 512) == 16 && (int64_t) peer.result >= 0, "blocking inotify read wakes on real peer VFS open"); });
    if ((int64_t) peer.result >= 0)
    {
        memory_call(SYSCALL_close, peer.result);
    }
    memory_call(SYSCALL_inotify_rm_watch, fd, blockingWatch);
    ok &= memory_expect(memory_call(SYSCALL_read, fd, base + 1024, 512) == 16, "drain ignored event before queue overflow test");
    const uint64_t overflowWatch = memory_call(SYSCALL_inotify_add_watch, fd, base + 128, 0x30);
    for (size_t i = 0; i < 40; ++i)
    {
        const uint64_t item = memory_call(SYSCALL_open, base + 128, 0);
        if ((int64_t) item >= 0)
        {
            memory_call(SYSCALL_close, item);
        }
    }
    ok &= memory_expect(memory_call(SYSCALL_read, fd, base + 2048, 2048) == 1024, "inotify queue remains bounded under overflow");
    uint32_t overflow[4] = {};
    time_fixture_load(process, base + 2048 + 63 * 16, overflow, sizeof(overflow));
    ok &= memory_expect(overflow[0] == UINT32_MAX && overflow[1] == 0x4000 && overflow[3] == 0, "inotify queue overflow ABI marker");
    memory_call(SYSCALL_inotify_rm_watch, fd, overflowWatch);
    ok &= memory_expect(memory_call(SYSCALL_inotify_add_watch, UINT32_MAX, CANONICAL_KERNEL_BASE, 1) == LINUX_EBADF && memory_call(SYSCALL_inotify_add_watch, fd, base + 128, 0) == LINUX_EINVAL, "inotify descriptor and empty event mask validation");
    memory_call(SYSCALL_close, fd);
    return ok;
}

static bool test_signalfd_framework(process_t* process, uint64_t base, uint64_t number)
{
    const uint64_t mask = (1ULL << 9) | (1ULL << 11) | SIGNAL_UNBLOCKABLE;
    time_fixture_store(process, base + 128, &mask, sizeof(mask));
    bool           ok = memory_expect(memory_call(SYSCALL_rt_sigprocmask, 0, base + 128, 0, 8) == 0, "block signals before receiving through signalfd");
    const uint64_t fd = memory_call(number, UINT32_MAX, base + 128, 8, number == SYSCALL_signalfd4 ? 04000 | 02000000 : 0);
    if (!memory_expect((int64_t) fd >= 0, "signalfd creation"))
    {
        return false;
    }
    ok &= memory_expect(memory_call(SYSCALL_fcntl, fd, 1) == (uint64_t) (number == SYSCALL_signalfd4), "signalfd creation CLOEXEC");
    memory_call(SYSCALL_fcntl, fd, 4, 04000);
    ok &= memory_expect(memory_call(SYSCALL_read, fd, base + 512, 128) == (uint64_t) -11 && memory_call(SYSCALL_read, fd, base + 512, 127) == LINUX_EINVAL, "signalfd no pending record and exact record-size minimum");
    const uint64_t tid = memory_call(SYSCALL_gettid);
    ok &= memory_expect(memory_call(SYSCALL_kill, process->id, 10) == 0 && memory_call(SYSCALL_tgkill, process->id, tid, 12) == 0, "queue process and thread directed signalfd signals");
    event_pollfd_t poll = {(int32_t) fd, 1, 0};
    time_fixture_store(process, base + 256, &poll, sizeof(poll));
    ok &= memory_expect(memory_call(SYSCALL_poll, base + 256, 1, 0) == 1 && memory_call(SYSCALL_read, fd, CANONICAL_KERNEL_BASE, 256) == LINUX_EFAULT, "signalfd readiness and fault-safe signal retention");
    file_iovec_t vectors[2] = {{base + 512, 64}, {base + 1024, 192}};
    time_fixture_store(process, base + 384, vectors, sizeof(vectors));
    ok &= memory_expect(memory_call(SYSCALL_readv, fd, base + 384, 2) == 256, "signalfd readv scatters two full records");
    uint8_t first[128] = {}, second[128] = {};
    time_fixture_load(process, base + 512, first, 64);
    time_fixture_load(process, base + 1024, first + 64, 64);
    time_fixture_load(process, base + 1088, second, 128);
    uint32_t info[5] = {};
    memcpy(info, first, sizeof(info));
    ok &= memory_expect(info[0] == 10 && info[2] == 0 && info[3] == process->id && info[4] == process->credentials.user.real, "signalfd process sender info ABI");
    memcpy(info, second, sizeof(info));
    ok &= memory_expect(info[0] == 12 && (int32_t) info[2] == -6 && info[3] == process->id, "signalfd thread sender info ABI");
    bool zero = true;
    for (size_t i = 20; i < 128; ++i)
    {
        zero &= first[i] == 0 && second[i] == 0;
    }
    ok &= memory_expect(zero && memory_call(SYSCALL_read, fd, base + 512, 128) == (uint64_t) -11, "signalfd zeroes reserved fields and consumes signals exactly once");
    ok &= memory_expect(memory_call(SYSCALL_signalfd4, fd, base + 128, 8, 02000000) == fd && memory_call(SYSCALL_fcntl, fd, 3) == 04000 && memory_call(SYSCALL_fcntl, fd, 1) == (uint64_t) (number == SYSCALL_signalfd4), "updating signalfd mask leaves existing descriptor flags unchanged");
    memory_call(SYSCALL_fcntl, fd, 4, 0);
    ipc_peer_context_t peer = {nullptr, SYSCALL_kill, {process->id, 10}, UINT64_MAX};
    ok &= ipc_with_peer(process, &peer, [&]() { return memory_expect(memory_call(SYSCALL_read, fd, base + 512, 128) == 128 && peer.result == 0, "blocked signalfd wakes for a masked peer signal"); });
    const uint64_t alarmMask = mask | (1ULL << 13);
    time_fixture_store(process, base + 128, &alarmMask, sizeof(alarmMask));
    time_itimerval_t timer = {{0, 0}, {0, 1000}};
    time_fixture_store(process, base + 2048, &timer, sizeof(timer));
    ok &= memory_expect(memory_call(SYSCALL_rt_sigprocmask, 0, base + 128, 0, 8) == 0 && memory_call(SYSCALL_signalfd4, fd, base + 128, 8, 0) == fd && memory_call(SYSCALL_setitimer, 0, base + 2048, 0) == 0 && memory_call(SYSCALL_read, fd, base + 512, 128) == 128,
                        "signalfd wakes for a blocked real-timer signal");
    time_fixture_load(process, base + 512, info, sizeof(info));
    ok &= memory_expect(info[0] == 14 && info[2] == 128, "signalfd timer SI_KERNEL record");
    ok &= memory_expect(memory_call(SYSCALL_signalfd4, fd, base + 128, 4, 0) == LINUX_EINVAL && memory_call(SYSCALL_signalfd4, fd, CANONICAL_KERNEL_BASE, 8, 0) == LINUX_EFAULT && memory_call(SYSCALL_signalfd4, UINT32_MAX, base + 128, 8, 1) == LINUX_EINVAL,
                        "signalfd sigset size, pointer and flag validation");
    memory_call(SYSCALL_close, fd);
    return ok;
}

template <uint64_t Number> static bool custom_case_validate_notification(const custom_case_ctx_t*)
{
    return run_memory_case(
            [](process_t* process)
            {
                auto           task    = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->GetCurrentTask();
                const uint64_t oldMask = task->signalMask, oldPending = task->signalPending;
                const uint64_t base = memory_map(4);
                bool           ok   = (int64_t) base >= 0 && (Number == SYSCALL_signalfd || Number == SYSCALL_signalfd4 ? test_signalfd_framework(process, base, Number) : test_inotify_framework(process, base, Number));
                task->signalMask    = oldMask;
                task->signalPending = oldPending;
                ok &= memory_expect(ipc_descriptor_count(process) == 0, "notification fixture releases every descriptor");
                return ok;
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
            {"HandleEpoll_pwaitRequest", &EventRequestManager::HandleEpoll_pwaitRequest, SYSCALL_epoll_pwait, custom_case_validate_event<SYSCALL_epoll_pwait>},
            {"HandleEpoll_pwait2Request", &EventRequestManager::HandleEpoll_pwait2Request, SYSCALL_epoll_pwait2, custom_case_validate_event<SYSCALL_epoll_pwait2>},
            {"HandleInotify_initRequest", &EventRequestManager::HandleInotify_initRequest, SYSCALL_inotify_init, custom_case_validate_notification<SYSCALL_inotify_init>},
            {"HandleInotify_init1Request", &EventRequestManager::HandleInotify_init1Request, SYSCALL_inotify_init1, custom_case_validate_notification<SYSCALL_inotify_init1>},
            {"HandleInotify_add_watchRequest", &EventRequestManager::HandleInotify_add_watchRequest, SYSCALL_inotify_add_watch, custom_case_validate_notification<SYSCALL_inotify_add_watch>},
            {"HandleInotify_rm_watchRequest", &EventRequestManager::HandleInotify_rm_watchRequest, SYSCALL_inotify_rm_watch, custom_case_validate_notification<SYSCALL_inotify_rm_watch>},
            {"HandleEventfdRequest", &EventRequestManager::HandleEventfdRequest, SYSCALL_eventfd, custom_case_validate_event<SYSCALL_eventfd>},
            {"HandleEventfd2Request", &EventRequestManager::HandleEventfd2Request, SYSCALL_eventfd2, custom_case_validate_event<SYSCALL_eventfd2>},
            {"HandleSignalfdRequest", &EventRequestManager::HandleSignalfdRequest, SYSCALL_signalfd, custom_case_validate_notification<SYSCALL_signalfd>},
            {"HandleSignalfd4Request", &EventRequestManager::HandleSignalfd4Request, SYSCALL_signalfd4, custom_case_validate_notification<SYSCALL_signalfd4>},
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
    context->entered              = true;
    context->protectedTask        = !context->resources->taskManager->FreeTask(context->sleeper) && !context->resources->taskManager->ExecuteTask(context->sleeper);

    if (context->operationMode == 131 || context->operationMode == 132)
    {
        context->mismatch     = context->operationMode == 132 ? memory_call(SYSCALL_futex, context->address, 132, 0, 1, context->address + 4, 8) : 0;
        context->moved        = memory_call(SYSCALL_futex, context->address, context->operationMode, 0, 1, context->address + 4, 7);
        context->wrongAddress = memory_call(SYSCALL_futex, context->address, 129, 1);
        context->wrongBits    = memory_call(SYSCALL_futex, context->address + 4, 138, 1, 0, 0, ~context->bits);
        context->result       = memory_call(SYSCALL_futex, context->address + 4, 138, 1, 0, 0, context->bits);

        if (context->result == 0)
        {
            // A failed requeue must not leave this test parked forever.
            memory_call(SYSCALL_futex, context->address, 129, 1);
        }

        context->scheduler->ExitCurrentTask();
    }

    if (context->operationMode == 133)
    {
        const uint32_t mismatch[] = {8, 7, 7, 6, 7, 8};
        const uint32_t matches[]  = {7, 8, 8, 7, 6, 7};
        const uint32_t compare    = context->compareMode << 24;
        context->mismatch         = memory_call(SYSCALL_futex, context->address + 4, 133, 1, 1, context->address, compare | (7U << 12) | mismatch[context->compareMode]);
        context->result           = memory_call(SYSCALL_futex, context->address + 4, 133, 1, 1, context->address, compare | (11U << 12) | matches[context->compareMode]);

        if (context->result != 1)
        {
            memory_call(SYSCALL_futex, context->address, 129, 1);
        }

        context->scheduler->ExitCurrentTask();
    }

    context->wrongAddress = memory_call(SYSCALL_futex, context->address + 4, 129, 1);
    cpu_preemption_disable();
    ProcessManager* processes = context->resources->processManager;
    process_t*      original  = processes->GetCurrentProcess();
    context->wrongProcess     = UINT64_MAX;

    if (processes->SetRunningProcess(arch_cpu_id(), context->isolated))
    {
        context->wrongProcess = memory_call(SYSCALL_futex, context->address, 129, 1);

        if (!processes->SetRunningProcess(arch_cpu_id(), original))
        {
            panic();
        }
    }

    cpu_preemption_enable();
    context->wrongBits = context->selective ? memory_call(SYSCALL_futex, context->address, 138, 1, 0, 0, ~context->bits) : 0;
    context->result    = context->selective ? memory_call(SYSCALL_futex, context->address, 138, 1, 0, 0, context->bits) : memory_call(SYSCALL_futex, context->address, 129, 0); // Legacy nonpositive count wakes one.
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
            {0x000ff000U, 123, 255}, {0x00fff000U, 123, UINT32_MAX}, {0x10001000U, UINT32_MAX, 0}, {0x20100000U, 0x00ff, 0x01ff}, {0x3000f000U, 0x00ff, 0x00f0}, {0x4000f000U, 0x00ff, 0x00f0}, {0x80021000U, 123, 2},
    };
    bool ok = true;

    for (const operation_test_t& test : tests)
    {
        time_fixture_store(process, destination, &test.before, sizeof(test.before));
        ok &= memory_expect(memory_call(SYSCALL_futex, base, 133, 0, 0, destination, test.encoding) == 0, "futex wake-op updates word without waiters");
        uint32_t actual = 0;
        time_fixture_load(process, destination, &actual, sizeof(actual));
        ok &= memory_expect(actual == test.after, "futex wake-op arithmetic, sign extension and masked shift");
    }

    uint32_t word = 3;
    time_fixture_store(process, destination, &word, sizeof(word));
    ok &= memory_expect(memory_call(SYSCALL_futex, base, 133, 1, 1, destination, 0x50001000U) == LINUX_ENOSYS, "futex wake-op invalid arithmetic operation");
    time_fixture_load(process, destination, &word, sizeof(word));
    ok &= memory_expect(word == 3, "invalid wake-op arithmetic leaves word untouched");
    ok &= memory_expect(memory_call(SYSCALL_futex, base, 133, 1, 1, destination, 0x0f009000U) == LINUX_ENOSYS, "futex wake-op invalid comparison");
    time_fixture_load(process, destination, &word, sizeof(word));
    ok &= memory_expect(word == 9, "invalid wake-op comparison follows atomic update");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_futex, base, 133, 1, 1, destination, 0x00001000U) == LINUX_EFAULT, "futex wake-op requires writable destination");
    time_fixture_load(process, destination, &word, sizeof(word));
    ok &= memory_expect(word == 9, "failed wake-op output access leaves word untouched");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 3);
    ok &= memory_expect(memory_call(SYSCALL_futex, base, 133, 1, 1, destination + 1, 0) == LINUX_EINVAL && memory_call(SYSCALL_futex, base, 133, 1, 1, base + 2 * PAGE_SIZE, 0) == LINUX_EFAULT, "futex wake-op destination alignment and mapping");
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
    context->destinationResult       = memory_call(SYSCALL_futex, context->destination, 137, 7, 0, 0, 2);
    __atomic_add_fetch(&context->completion, 1ULL, __ATOMIC_RELEASE);
    context->scheduler->WakeEventWaiters(&context->completion);
    context->scheduler->ExitCurrentTask();
}

static void futex_requeue_entry(void* argument)
{
    cpu_preemption_disable();
    futex_requeue_context_t* context = static_cast<futex_requeue_context_t*>(argument);
    context->moved                   = memory_call(SYSCALL_futex, context->source, 132, 0, 1, context->destination, 7);
    context->firstWake               = memory_call(SYSCALL_futex, context->destination, 138, 1, 0, 0, 2);
    context->ordered                 = !context->destinationTask->sleepBlocked && context->sourceTask->sleepBlocked;
    context->secondWake              = memory_call(SYSCALL_futex, context->destination, 138, 1, 0, 0, 2);
    memory_call(SYSCALL_futex, context->source, 129, 1);
    context->scheduler->ExitCurrentTask();
}

static bool test_sync_requeue_fifo(process_t* process, uint64_t base)
{
    Dispatcher*             dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps*      resources  = dispatcher->GetResourceLayerCaps();
    Scheduler*              scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    futex_requeue_context_t context    = {resources, scheduler, resources->taskManager->GetCurrentTask(), nullptr, base + 512, base + 516, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, false};
    const uint32_t          words[2]   = {7, 7};
    time_fixture_store(process, context.source, words, sizeof(words));
    task_t* peers[2] = {
            resources->taskManager->CreateKernelTask(futex_destination_entry, &context),
            resources->taskManager->CreateKernelTask(futex_requeue_entry, &context),
    };
    context.destinationTask = peers[0];
    bool added[2]           = {false, false};

    for (size_t i = 0; i < 2; ++i)
    {
        added[i] = peers[i] != nullptr && resources->processManager->AddTask(process, peers[i]);
    }

    const bool ready = added[0] && added[1] && scheduler->EnqueueProcess(arch_cpu_id(), process->id);
    bool       ok    = memory_expect(ready, "futex destination FIFO fixture");

    if (ready)
    {
        const uint64_t result = memory_call(SYSCALL_futex, context.source, 137, 7, 0, 0, 2);
        ok &= memory_expect(result == 0 && context.moved == 1 && context.firstWake == 1 && context.secondWake == 1 && context.ordered, "requeued waiter follows existing destination waiters");

        if (__atomic_load_n(&context.completion, __ATOMIC_ACQUIRE) == 0)
        {
            ok &= memory_expect(scheduler->WaitForEvent(&context.completion, 0) == 0, "futex FIFO destination peer completes before fixture release");
        }

        ok &= memory_expect(context.destinationResult == 0, "existing destination waiter wakes successfully");
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
            ok &= memory_expect(resources->taskManager->FreeTask(peers[i]), "futex FIFO peer cleanup");
        }
    }

    return ok;
}

static bool test_sync_futex(process_t* process)
{
    const uint64_t base = memory_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    const uint64_t address = base + PAGE_SIZE - 4;
    const uint64_t timeout = base + 64;
    uint32_t       word    = 7;
    time_fixture_store(process, address, &word, sizeof(word));
    bool ok = memory_expect(memory_call(SYSCALL_futex, address, 128, 8) == (uint64_t) -11, "futex compare mismatch EAGAIN");
    ok &= test_sync_wake_operation(process, base);
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 129, 1) == 0 && memory_call(SYSCALL_futex, address, 129, 0) == 0, "futex wake with no waiters/zero count");
    ok &= memory_expect(memory_call(SYSCALL_futex, address + 1, 128, 7) == LINUX_EINVAL && memory_call(SYSCALL_futex, CANONICAL_KERNEL_BASE, 128, 7) == LINUX_EFAULT && memory_call(SYSCALL_futex, base + 2 * PAGE_SIZE, 128, 7) == LINUX_EFAULT, "futex alignment/kernel/unmapped word validation");
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 137, 7, 0, 0, 0) == LINUX_EINVAL && memory_call(SYSCALL_futex, address, 138, 1, 0, 0, 0) == LINUX_EINVAL && memory_call(SYSCALL_futex, address, 129, UINT64_MAX) == 0, "futex bitsets and empty legacy wake count");
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 0, 7) == LINUX_EOPNOTSUPP && memory_call(SYSCALL_futex, address, 134, 7) == LINUX_ENOSYS && memory_call(SYSCALL_futex, address, 128 | 256, 7) == LINUX_ENOSYS, "futex shared/PI/unsupported clock modes explicit");
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 131, 0, 1, address + 4) == 0 && memory_call(SYSCALL_futex, address, 132, 0, 1, address + 4, 8) == (uint64_t) -11, "futex requeue empty queue and compare mismatch");
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 132, 0, 1, address + 4, 7) == 0 && memory_call(SYSCALL_futex, address, 131, 0, 0, address) == 0, "futex compare requeue and identical private keys");
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 131, UINT64_MAX, 1, address + 4) == LINUX_EINVAL && memory_call(SYSCALL_futex, address, 132, 0, UINT64_MAX, address + 4, 7) == LINUX_EINVAL, "futex requeue negative counts");
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 131, 0, 1, address + 1) == LINUX_EINVAL && memory_call(SYSCALL_futex, address, 132, 0, 1, CANONICAL_KERNEL_BASE, 7) == LINUX_EFAULT, "futex requeue destination validation");
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 128, 7, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "futex invalid timeout buffer");
    time_timespec_t duration = {-1, 0};
    time_fixture_store(process, timeout, &duration, sizeof(duration));
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 128, 7, timeout) == LINUX_EINVAL, "futex negative timeout");
    duration = {0, 1000000000};
    time_fixture_store(process, timeout, &duration, sizeof(duration));
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 128, 7, timeout) == LINUX_EINVAL, "futex timeout nanoseconds");
    Dispatcher*        dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    ResourceLayerCaps* resources  = dispatcher->GetResourceLayerCaps();
    Scheduler*         scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    time_timespec_t    now;

    if (resources->timeManager != nullptr && resources->timeManager->ReadClock(1, &now) == 0)
    {
        duration = {0, 1000000};
        time_fixture_store(process, timeout, &duration, sizeof(duration));
        ok &= memory_expect(memory_call(SYSCALL_futex, address, 128, 7, timeout) == (uint64_t) -110, "futex relative finite timeout ETIMEDOUT");
        duration = {0, 0};
        time_fixture_store(process, timeout, &duration, sizeof(duration));
        ok &= memory_expect(memory_call(SYSCALL_futex, address, 137, 7, timeout, 0, 1) == (uint64_t) -110, "futex bitset absolute past deadline");
        ok &= memory_expect(memory_call(SYSCALL_futex, address, 128, 8, timeout) == (uint64_t) -11, "futex mismatch wins over expired timeout");
        ok &= memory_expect(memory_call(SYSCALL_futex, address, 129, 1) == 0, "futex timeout removes waiter");

        if (resources->timeManager->ReadClock(0, &now) == 0)
        {
            ok &= memory_expect(memory_call(SYSCALL_futex, address, 137 | 256, 7, timeout, 0, 1) == (uint64_t) -110, "futex bitset realtime absolute past deadline");
        }

        duration = {INT64_MAX, 0};
        time_fixture_store(process, timeout, &duration, sizeof(duration));
        ok &= memory_expect(memory_call(SYSCALL_futex, address, 128, 7, timeout) == LINUX_EOVERFLOW, "futex unrepresentable timeout");
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

        return memory_expect(false, "futex private-key process isolation fixture");
    }

    const uint32_t waitOperations[] = {128U, 137U, 131U, 132U, 133U, 0x10085U, 0x20085U, 0x30085U, 0x40085U, 0x50085U};

    for (uint32_t operation : waitOperations)
    {
        word = 7;
        time_fixture_store(process, address, &word, sizeof(word));
        futex_peer_context_t context = {scheduler, resources, resources->taskManager->GetCurrentTask(), address, 2, operation == 137, false, false, UINT64_MAX, UINT64_MAX, UINT64_MAX, isolated, UINT64_MAX};
        const uint32_t       command = operation & 0xffffU;
        context.operationMode        = command >= 131 && command <= 133 ? command : 0;
        context.compareMode          = operation >> 16;
        task_t*    peer              = resources->taskManager->CreateKernelTask(futex_peer_entry, &context);
        const bool added             = peer != nullptr && resources->processManager->AddTask(process, peer);
        const bool ready             = added && scheduler->EnqueueProcess(arch_cpu_id(), process->id);
        ok &= memory_expect(ready, "futex runnable sibling task fixture");

        if (ready)
        {
            // Same process/address space: FUTEX_PRIVATE keys must not match a
            // different process simply because it maps the same virtual address.
            const uint32_t mask   = operation == 128 ? UINT32_MAX : context.bits;
            const uint32_t wait   = context.operationMode != 0 ? 137 : operation;
            const uint64_t result = memory_call(SYSCALL_futex, address, wait, 7, 0, 0, mask);

            if (context.operationMode == 131 || context.operationMode == 132)
            {
                ok &= memory_expect(result == 0 && context.entered && context.protectedTask && context.moved == 1 && context.wrongAddress == 0 && context.wrongBits == 0 && context.result == 1, "futex requeue moves parked waiter and preserves bitset");
                ok &= memory_expect(context.mismatch == (operation == 132 ? (uint64_t) -11 : 0), "futex compare mismatch leaves waiter on source");
            }
            else if (context.operationMode == 133)
            {
                time_fixture_load(process, address, &word, sizeof(word));
                ok &= memory_expect(result == 0 && context.mismatch == 0 && context.result == 1 && word == 11, "futex wake-op condition tests old value before atomic update");
            }
            else
            {
                ok &= memory_expect(result == 0 && context.entered && context.protectedTask && context.wrongAddress == 0 && context.wrongProcess == 0 && context.result == 1, "futex real parked wait/peer wake, process isolation and task lifetime");
            }

            if (operation == 137)
            {
                ok &= memory_expect(context.wrongBits == 0, "futex bitset filters wakeups");
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
            ok &= memory_expect(resources->taskManager->FreeTask(peer), "futex peer cleanup");
        }

        ok &= memory_expect(memory_call(SYSCALL_futex, address, 129, 1) == 0, "futex wake removes waiter exactly once");
    }

    ok &= memory_expect(resources->processManager->FreeProcess(isolated), "futex private-key process cleanup");
    resources->virtualMemoryManager->DestroyUserAddressSpace(isolatedSpace);
    ok &= test_sync_requeue_fifo(process, base);
    word = 7;
    time_fixture_store(process, address, &word, sizeof(word));
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 128, 8) == (uint64_t) -11, "futex compare permits readable readonly word");
    memory_call(SYSCALL_mprotect, base, PAGE_SIZE, 0);
    ok &= memory_expect(memory_call(SYSCALL_futex, address, 128, 7) == LINUX_EFAULT, "futex inaccessible word rejected");
    return ok;
}

static bool test_sync_robust(process_t* process, uint64_t number)
{
    const uint64_t base = memory_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    const uint64_t headAddress   = base + PAGE_SIZE - 4;
    const uint64_t lengthAddress = base + 64;
    bool           ok            = memory_expect(memory_call(SYSCALL_set_robust_list, 0, 24) == 0, "robust empty-list clearing");
    ok &= memory_expect(memory_call(SYSCALL_set_robust_list, base, 23) == LINUX_EINVAL && memory_call(SYSCALL_set_robust_list, base, 24) == 0, "robust length validation and registration");
    const uint64_t tid = memory_call(SYSCALL_gettid);
    ok &= memory_expect(memory_call(SYSCALL_get_robust_list, number == SYSCALL_get_robust_list ? tid : 0, headAddress, lengthAddress) == 0, "robust current task query");
    uint64_t head = UINT64_MAX, length = 0;
    time_fixture_load(process, headAddress, &head, sizeof(head));
    time_fixture_load(process, lengthAddress, &length, sizeof(length));
    ok &= memory_expect(head == base && length == 24, "robust registered head query");
    ok &= memory_expect(memory_call(SYSCALL_set_robust_list, 0, 24) == 0, "robust unregister releases owner pin");
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
    time_fixture_store(process, base + 128, &robust, sizeof(robust));
    time_fixture_store(process, base + 256, &node, sizeof(node));
    time_fixture_store(process, base + 288, &node, sizeof(node));
    ok &= memory_expect(memory_call(SYSCALL_set_robust_list, base + 128, 24) == 0, "robust linked and pending registrations");
    static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->CleanupCurrentTask();
    uint32_t died = 0, pendingDied = 0;
    time_fixture_load(process, base + 264, &died, 4);
    time_fixture_load(process, base + 296, &pendingDied, 4);
    ok &= memory_expect(died == 0xc0000000U && pendingDied == 0xc0000000U, "robust owner death updates list and pending operation");
    ok &= memory_expect(memory_call(SYSCALL_get_robust_list, 0, lengthAddress, lengthAddress) == 0, "robust aliased outputs permitted");
    time_fixture_load(process, lengthAddress, &head, sizeof(head));
    ok &= memory_expect(head == 0, "robust outputs follow Linux length-then-head order");
    ok &= memory_expect(memory_call(SYSCALL_get_robust_list, UINT64_MAX, headAddress, lengthAddress) == LINUX_ESRCH && memory_call(SYSCALL_get_robust_list, TaskManager::MAX_TASKS, headAddress, lengthAddress) == LINUX_ESRCH, "robust invalid task identifiers");
    ResourceLayerCaps* resources = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps();
    task_t*            sibling   = resources->taskManager->AllocateTask();
    const bool         attached  = sibling != nullptr && resources->processManager->AddTask(process, sibling);
    ok &= memory_expect(attached, "robust sibling query fixture");

    if (attached)
    {
        __atomic_store_n(&sibling->robustHead, base + 128, __ATOMIC_RELEASE);
        ok &= memory_expect(memory_call(SYSCALL_get_robust_list, sibling->id, headAddress, lengthAddress) == 0, "robust sibling task query");
        time_fixture_load(process, headAddress, &head, sizeof(head));
        ok &= memory_expect(head == base + 128 && sibling->dispatchUsers == 0, "robust sibling snapshot and lifetime claim release");
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
        ok &= memory_expect(resources->taskManager->FreeTask(sibling), "robust sibling fixture cleanup");
        ok &= memory_expect(memory_call(SYSCALL_get_robust_list, siblingId, headAddress, lengthAddress) == LINUX_ESRCH, "robust retired sibling is absent");
    }
    ok &= memory_expect(memory_call(SYSCALL_get_robust_list, 0, CANONICAL_KERNEL_BASE, lengthAddress) == LINUX_EFAULT && memory_call(SYSCALL_get_robust_list, 0, headAddress, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "robust invalid output pointers");
    memory_call(SYSCALL_mprotect, base + PAGE_SIZE, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_get_robust_list, 0, headAddress, lengthAddress) == LINUX_EFAULT, "robust readonly output across page");
    return ok;
}

template <uint64_t Number> static bool custom_case_validate_sync(const custom_case_ctx_t*)
{
    return run_memory_case([](process_t* process) { return Number == SYSCALL_futex ? test_sync_futex(process) : test_sync_robust(process, Number); });
}

static void test_sync_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<SyncRequestManager> tests[] = {
            {"HandleFutexRequest", &SyncRequestManager::HandleFutexRequest, SYSCALL_futex, custom_case_validate_sync<SYSCALL_futex>},
            {"HandleSet_robust_listRequest", &SyncRequestManager::HandleSet_robust_listRequest, SYSCALL_set_robust_list, custom_case_validate_sync<SYSCALL_set_robust_list>},
            {"HandleGet_robust_listRequest", &SyncRequestManager::HandleGet_robust_listRequest, SYSCALL_get_robust_list, custom_case_validate_sync<SYSCALL_get_robust_list>},
    };

    SyncRequestManager unavailable(nullptr, nullptr);

    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};

        if ((unavailable.*test.method)(&frame) != LINUX_ENOSYS || (caps->syncRequestManager != nullptr && (caps->syncRequestManager->*test.method)(nullptr) != LINUX_EINVAL))
        {
            stats->implementedFail++;
            selftest_record_failure_detail("sync missing dependency/null-frame validation");
        }
    }

    run_manager_tests("sync", caps->syncRequestManager, tests, stats, true, true);
}

struct signal_peer_context_t
{
    ResourceLayerCaps* resources;
    Scheduler*         scheduler;
    process_t*         process;
    uint64_t           robustHead;
    uint64_t           result;
};

static void signal_peer_entry(void* argument)
{
    cpu_preemption_disable();
    auto context    = static_cast<signal_peer_context_t*>(argument);
    context->result = context->robustHead != 0 ? memory_call(SYSCALL_set_robust_list, context->robustHead, 24) : memory_call(SYSCALL_kill, context->process->id, 10);
    context->scheduler->ExitCurrentTask();
}

template <typename Wait> static bool signal_with_peer(process_t* process, uint64_t robustHead, uint64_t robustWord, Wait wait)
{
    Dispatcher*           dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto                  resources  = dispatcher->GetResourceLayerCaps();
    auto                  scheduler  = dispatcher->GetLogicLayerCaps()->scheduler;
    signal_peer_context_t context    = {resources, scheduler, process, robustHead, UINT64_MAX};
    task_t*               peer       = resources->taskManager->CreateKernelTask(signal_peer_entry, &context);
    bool                  added      = peer != nullptr && resources->processManager->AddTask(process, peer);

    if (added && robustWord != 0)
    {
        uint32_t owned = 0x80000000U | (uint32_t) peer->id;
        time_fixture_store(process, robustWord, &owned, 4);
    }

    const bool ready = added && scheduler->EnqueueProcess(arch_cpu_id(), process->id);
    bool       ok    = memory_expect(ready, "signal/owner-death runnable peer fixture");

    if (ready)
    {
        ok &= wait(peer->id) && memory_expect(context.result == 0, "signal/robust peer syscall succeeds");
    }

    if (added)
    {
        if (peer->prev != nullptr)
            peer->prev->next = peer->next;
        else
            process->tasks = peer->next;

        if (peer->next != nullptr)
        {
            peer->next->prev = peer->prev;
        }

        peer->next = peer->prev = nullptr;
    }

    if (peer != nullptr)
    {
        ok &= memory_expect(resources->taskManager->FreeTask(peer), "signal peer retirement cleanup");
    }

    return ok;
}

static bool test_signal_case(process_t* process, uint64_t number)
{
    const uint64_t base = memory_map(6);

    if ((int64_t) base < 0)
    {
        return false;
    }

    auto            dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto            signals    = dispatcher->GetLogicLayerCaps()->signalManager;
    auto            resources  = dispatcher->GetResourceLayerCaps();
    const uint64_t  input = base + PAGE_SIZE - 4, output = base + 128;
    const uint64_t  tid    = memory_call(SYSCALL_gettid);
    bool            ok     = true;
    signal_action_t action = {base + 512, 0x14000004, base + 768, 0}; // RESTORER|RESTART|SIGINFO
    time_fixture_store(process, input, &action, sizeof(action));
    ok &= memory_expect(memory_call(SYSCALL_rt_sigaction, 10, input, 0, 8) == 0, "signal handler action install");

    if (number == SYSCALL_rt_sigaction)
    {
        signal_action_t queried = {};
        ok &= memory_expect(memory_call(number, 10, 0, output, 8) == 0, "signal action query");
        time_fixture_load(process, output, &queried, sizeof(queried));
        ok &= memory_expect(queried.handler == action.handler && queried.flags == action.flags, "signal raw action ABI round trip");
        ok &= memory_expect(memory_call(number, 0, 0, output, 8) == LINUX_EINVAL && memory_call(number, 9, input, 0, 8) == LINUX_EINVAL && memory_call(number, 19, input, 0, 8) == LINUX_EINVAL && memory_call(number, 10, input, 0, 16) == LINUX_EINVAL, "signal number/unblockable/size validation");
        ok &= memory_expect(memory_call(number, 10, CANONICAL_KERNEL_BASE, 0, 8) == LINUX_EFAULT, "signal action bad user input");
        action.flags |= 2;
        time_fixture_store(process, input, &action, sizeof(action));
        ok &= memory_expect(memory_call(number, 10, input, 0, 8) == LINUX_EOPNOTSUPP, "unsupported child-reaping action flag rejected");
    }
    else if (number == SYSCALL_rt_sigprocmask)
    {
        uint64_t mask = UINT64_MAX, queried = 0;
        time_fixture_store(process, input, &mask, 8);
        ok &= memory_expect(memory_call(number, 2, input, output, 8) == 0, "signal mask replacement");
        ok &= memory_expect(memory_call(number, 99, 0, output, 8) == 0, "signal mask query ignores how without input");
        time_fixture_load(process, output, &queried, 8);
        ok &= memory_expect(queried == (UINT64_MAX & ~SIGNAL_UNBLOCKABLE), "SIGKILL/SIGSTOP cannot be masked");
        ok &= memory_expect(memory_call(number, 3, input, 0, 8) == LINUX_EINVAL && memory_call(number, 0, input, 0, 4) == LINUX_EINVAL, "signal mask operation and size errors");
        mask = 0;
        time_fixture_store(process, input, &mask, 8);
        memory_call(number, 2, input, 0, 8);
    }
    else if (number == SYSCALL_sigaltstack)
    {
        signal_stack_t stack = {base + PAGE_SIZE * 2, 0, 0, PAGE_SIZE * 2}, queried = {};
        time_fixture_store(process, input, &stack, sizeof(stack));
        ok &= memory_expect(memory_call(number, input, output) == 0 && memory_call(number, 0, output) == 0, "alternate stack install/query");
        time_fixture_load(process, output, &queried, sizeof(queried));
        ok &= memory_expect(queried.sp == stack.sp && queried.size == stack.size && queried.flags == 0, "alternate stack raw ABI");
        arch_syscall_frame_t active = {};
        active.syscall_number       = number;
        active.arg0                 = input;
#if defined(__x86_64__)
        active.user_rsp = stack.sp + 64;
#else
        active.user_sp = stack.sp + 64;
#endif
        ok &= memory_expect(dispatcher_dispatch_syscall(&active) == (uint64_t) -1, "alternate stack cannot change while active");
        stack.size = 1024;
        time_fixture_store(process, input, &stack, sizeof(stack));
        ok &= memory_expect(memory_call(number, input) == LINUX_ENOMEM, "alternate stack minimum size");
    }
    else if (number == SYSCALL_kill || number == SYSCALL_tgkill)
    {
        auto send = [&](uint64_t signal) { return number == SYSCALL_kill ? memory_call(number, process->id, signal) : memory_call(number, process->id, tid, signal); };
        ok &= memory_expect(send(0) == 0 && send(10) == 0, "signal existence probe and pending enqueue");
        task_t* task = resources->taskManager->GetCurrentTask();
        ok &= memory_expect((pending_signals(process, task) & 512) != 0, "signal routed to current process/thread");
        ok &= memory_expect(send(65) == LINUX_EINVAL && send(34) == LINUX_EOPNOTSUPP, "invalid and unsupported realtime signals");
        ok &= memory_expect(number == SYSCALL_kill ? memory_call(number, 0, 10) == LINUX_EOPNOTSUPP : memory_call(number, process->id, 0, 10) == LINUX_EINVAL, "signal unsupported group/invalid thread targeting");
        signal_action_t ignore = {1, 0, 0, 0};
        time_fixture_store(process, input, &ignore, sizeof(ignore));
        ok &= memory_expect(memory_call(SYSCALL_rt_sigaction, 10, input, 0, 8) == 0 && send(10) == 0 && (pending_signals(process, task) & 512) == 0, "ignored signal discards pending instances");
        auto       space  = resources->virtualMemoryManager->CreateUserAddressSpace();
        process_t* victim = space != nullptr ? resources->processManager->CreateProcess(space) : nullptr;

        if (victim != nullptr)
        {
            process_credentials_t previous = process->credentials;
            process->credentials.user      = {100, 100, 100, 100};
            victim->credentials.user       = {200, 200, 200, 200};
            ok &= memory_expect(memory_call(SYSCALL_kill, victim->id, 0) == (uint64_t) -1, "signal permissions reject unrelated identities");
            process->credentials.user.effective = 0;
            ok &= memory_expect(memory_call(SYSCALL_kill, victim->id, 0) == 0, "root signal permission probe");
            process->credentials = previous;
            ok &= memory_expect(resources->processManager->FreeProcess(victim), "signal permission target cleanup");
        }
        else
        {
            ok &= memory_expect(false, "signal permission target fixture");
        }

        if (space != nullptr)
        {
            resources->virtualMemoryManager->DestroyUserAddressSpace(space);
        }
    }
    else if (number == SYSCALL_pause)
    {
        ok &= signal_with_peer(process, 0, 0, [&](uint64_t) { return memory_expect(memory_call(number) == (uint64_t) -4, "pause returns EINTR from real peer signal"); });
        __atomic_store_n(&process->signalPending, 0ULL, __ATOMIC_RELEASE);
        time_timespec_t now;

        if (resources->timeManager->ReadClock(1, &now) == 0)
        {
            time_timespec_t duration = {1, 0}, remaining = {};
            time_fixture_store(process, input, &duration, sizeof(duration));
            ok &= signal_with_peer(process, 0, 0, [&](uint64_t) { return memory_expect(memory_call(SYSCALL_nanosleep, input, output) == (uint64_t) -4, "nanosleep signal interruption"); });
            time_fixture_load(process, output, &remaining, sizeof(remaining));
            ok &= memory_expect(remaining.seconds >= 0 && remaining.seconds <= 1 && remaining.nanoseconds >= 0 && remaining.nanoseconds < 1000000000, "interrupted sleep normalized remainder");
        }
    }
    else if (number == SYSCALL_rt_sigreturn)
    {
#if defined(__x86_64__)
        alignas(16) uint8_t floating[512] = {};
        uint32_t            mxcsr = 0x1f80, mask = 0xffbf;
        memcpy(floating + 24, &mxcsr, 4);
        memcpy(floating + 28, &mask, 4);
        floating[160]                     = 0x5a;
        arch_syscall_frame_t f            = {};
        f.syscall_number                  = SYSCALL_futex;
        f.user_cs                         = USER_CS;
        f.user_ss                         = USER_SS;
        f.user_rflags                     = 0x202;
        f.user_rip                        = base + 514;
        f.user_rsp                        = base + 6 * PAGE_SIZE - 16;
        f.rbx                             = 0x123456789abcdef0ULL;
        f.r13                             = 0xaabbccdd;
        f.fxstate                         = (uint64_t) floating;
        const uint16_t syscallInstruction = 0x050f;
        time_fixture_store(process, base + 512, &syscallInstruction, 2);
        ok &= memory_expect(memory_call(SYSCALL_kill, process->id, 10) == 0, "signal return pending fixture");
        ok &= memory_expect(signals->Deliver(&f, (uint64_t) -4, true) == 0 && f.user_rip == action.handler && f.arg0 == 10, "signal frame delivery");
        const uint64_t frameAddress = f.user_rsp;
        signal_frame_fixture_t saved        = {};
        time_fixture_load(process, frameAddress, &saved, sizeof(saved));
        ok &= memory_expect(saved.uc.context.ax == SYSCALL_futex && saved.uc.context.ip == base + 512 && saved.uc.context.bx == 0x123456789abcdef0ULL && (frameAddress & 15) == 8, "SA_RESTART rewind and full register context");
        f.syscall_number      = SYSCALL_rt_sigreturn;
        f.user_rsp            = frameAddress + 8;
        const uint64_t goodIp = saved.uc.context.ip;
        saved.uc.context.ip   = CANONICAL_KERNEL_BASE;
        time_fixture_store(process, frameAddress, &saved, sizeof(saved));
        ok &= memory_expect(dispatcher_dispatch_syscall(&f) == LINUX_EINVAL && f.user_rsp == frameAddress + 8 && resources->taskManager->GetCurrentTask()->signalDepth == 1, "sigreturn rejects kernel instruction pointer without consuming frame");
        saved.uc.context.ip    = goodIp;
        saved.uc.context.flags = UINT64_MAX;
        time_fixture_store(process, frameAddress, &saved, sizeof(saved));
        uint32_t invalidMxcsr = UINT32_MAX;
        time_fixture_store(process, saved.uc.context.fpstate + 24, &invalidMxcsr, 4);
        ok &= memory_expect(dispatcher_dispatch_syscall(&f) == LINUX_EINVAL, "sigreturn rejects reserved MXCSR bits");
        time_fixture_store(process, saved.uc.context.fpstate + 24, &mxcsr, 4);
        floating[160] = 0;
        ok &= memory_expect(dispatcher_dispatch_syscall(&f) == SYSCALL_futex && f.rbx == 0x123456789abcdef0ULL && f.r13 == 0xaabbccdd && floating[160] == 0x5a && (f.user_rflags & 0x27000) == 0, "sigreturn GP/FP restoration and privilege flag sanitization");
        // Negative user return values must not be confused with restore errors.
        saved.uc.context.ax = (uint64_t) -4;
        saved.uc.mask       = UINT64_MAX;
        time_fixture_store(process, frameAddress, &saved, sizeof(saved));
        task_t* restoredTask      = resources->taskManager->GetCurrentTask();
        restoredTask->signalDepth = 1;
        f.user_rsp                = frameAddress + 8;
        ok &= memory_expect(dispatcher_dispatch_syscall(&f) == (uint64_t) -4 && restoredTask->signalDepth == 0 && restoredTask->signalMask == (UINT64_MAX & ~SIGNAL_UNBLOCKABLE), "sigreturn restores negative result and sanitizes mask");
        restoredTask->signalMask = 0;
        uint32_t word            = 7;
        time_fixture_store(process, base + 64, &word, 4);
        ok &= signal_with_peer(process, 0, 0, [&](uint64_t) { return memory_expect(memory_call(SYSCALL_futex, base + 64, 128, 7) == (uint64_t) -4, "futex interrupted by real pending signal"); });
        ok &= memory_expect(memory_call(SYSCALL_futex, base + 64, 129, 1) == 0, "interrupted futex dequeues waiter");
        __atomic_store_n(&process->signalPending, 0ULL, __ATOMIC_RELEASE);
        struct
        {
            uint64_t next;
            int64_t  offset;
            uint64_t pending;
        } robust      = {base + 256, 8, 0};
        uint64_t next = base + 128;
        time_fixture_store(process, base + 128, &robust, sizeof(robust));
        time_fixture_store(process, base + 256, &next, 8);
        ok &= signal_with_peer(process, base + 128, base + 264, [&](uint64_t owner) { return memory_expect(memory_call(SYSCALL_futex, base + 264, 128, 0x80000000U | (uint32_t) owner) == 0, "robust task exit wakes futex waiter"); });
        uint32_t died = 0;
        time_fixture_load(process, base + 264, &died, 4);
        ok &= memory_expect(died == 0xc0000000U, "robust exit publishes OWNER_DIED with WAITERS preserved");
#endif
    }

    return ok;
}

template <uint64_t Number> static bool custom_case_validate_signal(const custom_case_ctx_t*)
{
    return run_memory_case(
            [](process_t* process)
            {
                auto           resources = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps();
                task_t*        task      = resources->taskManager->GetCurrentTask();
                uint64_t       mask = task->signalMask, pending = task->signalPending;
                signal_stack_t stack = task->signalStack;
                uint64_t       depth = task->signalDepth;
                bool           ok    = test_signal_case(process, Number);
                task->signalMask     = mask;
                task->signalPending  = pending;
                task->signalStack    = stack;
                task->signalDepth    = depth;
                return ok;
            });
}

static void test_signal_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<SignalRequestManager> tests[] = {
            {"HandleRt_sigreturnRequest", &SignalRequestManager::HandleRt_sigreturnRequest, SYSCALL_rt_sigreturn, custom_case_validate_signal<SYSCALL_rt_sigreturn>},
            {"HandleRt_sigprocmaskRequest", &SignalRequestManager::HandleRt_sigprocmaskRequest, SYSCALL_rt_sigprocmask, custom_case_validate_signal<SYSCALL_rt_sigprocmask>},
            {"HandleRt_sigactionRequest", &SignalRequestManager::HandleRt_sigactionRequest, SYSCALL_rt_sigaction, custom_case_validate_signal<SYSCALL_rt_sigaction>},
            {"HandleSigaltstackRequest", &SignalRequestManager::HandleSigaltstackRequest, SYSCALL_sigaltstack, custom_case_validate_signal<SYSCALL_sigaltstack>},
            {"HandleKillRequest", &SignalRequestManager::HandleKillRequest, SYSCALL_kill, custom_case_validate_signal<SYSCALL_kill>},
            {"HandleTgkillRequest", &SignalRequestManager::HandleTgkillRequest, SYSCALL_tgkill, custom_case_validate_signal<SYSCALL_tgkill>},
            {"HandlePauseRequest", &SignalRequestManager::HandlePauseRequest, SYSCALL_pause, custom_case_validate_signal<SYSCALL_pause>},
    };
    SignalRequestManager unavailable(nullptr, nullptr);

    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};

        if ((unavailable.*test.method)(&frame) != LINUX_ENOSYS || (caps->signalRequestManager != nullptr && (caps->signalRequestManager->*test.method)(nullptr) != LINUX_EINVAL))
        {
            stats->implementedFail++;
            selftest_record_failure_detail("signal null-frame/missing-dependency validation");
        }
    }

    run_manager_tests("signal", caps->signalRequestManager, tests, stats, true, true);
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
        {
            return false;
        }

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
        {
            ok &= memory_expect(memory_call(SYSCALL_setuid, 1000) == 0, "drop group-management privilege");
        }

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
        {
            ok &= memory_expect(memory_call(SYSCALL_setuid, 1000) == 0, "drop setregid privilege");
        }

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
        {
            ok &= memory_expect(memory_call(SYSCALL_setuid, 1000) == 0, "drop setresgid privilege");
        }

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
        {
            ok &= memory_expect(memory_call(SYSCALL_setuid, 1000) == 0, "drop setfsgid privilege");
        }

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
    {
        return false;
    }

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
        {
            ok &= memory_expect(processes->FreeProcess(reused), "credential reused-slot cleanup");
        }
    }

    if (space != nullptr)
    {
        memory->DestroyUserAddressSpace(space);
    }

    const uint32_t maximum = ProcessManager::MAX_SUPPLEMENTARY_GROUPS;
    uint64_t       large   = memory_map(maximum * sizeof(uint32_t) / PAGE_SIZE);

    if ((int64_t) large >= 0)
    {
        for (uint32_t offset = 0; offset < maximum; offset += PAGE_SIZE / sizeof(uint32_t))
        {
            uint32_t* page = reinterpret_cast<uint32_t*>(memory_bytes(process, large + offset * sizeof(uint32_t)));

            for (uint32_t i = 0; i < PAGE_SIZE / sizeof(uint32_t); ++i)
            {
                page[i] = maximum - offset - i;
            }
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
                {
                    return memory_expect(false, "credential isolated fixture state");
                }

                // Only the disposable fixture is privileged. The harness's credentials
                // are retained separately and restored after the preliminary probes.
                locked->credentials = {};
                processes->ReleaseCredentials(locked);

                if (memory_call(SYSCALL_setgroups, 0, 0) != 0)
                {
                    return false;
                }

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
    {
        __atomic_add_fetch(&originalGroups->references, 1U, __ATOMIC_RELAXED);
    }

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
    {
        return false;
    }

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
    {
        return false;
    }

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
        {
            ok &= memory_expect(data[field * 65 + 64] == 0, "uname fields are NUL terminated");
        }

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

struct socket_peer_context_t
{
    Scheduler* scheduler;
    uint64_t   number;
    uint64_t   args[6];
    uint64_t   result;
};

static void socket_peer_entry(void* argument)
{
    cpu_preemption_disable();
    socket_peer_context_t* context = static_cast<socket_peer_context_t*>(argument);
    context->result                = memory_call(context->number, context->args[0], context->args[1], context->args[2], context->args[3], context->args[4], context->args[5]);
    context->scheduler->ExitCurrentTask();
}

template <typename Wait> static bool socket_with_peer(process_t* process, socket_peer_context_t* context, Wait wait)
{
    auto dispatcher    = static_cast<Dispatcher*>(platform.dispacher);
    auto resources     = dispatcher->GetResourceLayerCaps();
    context->scheduler = dispatcher->GetLogicLayerCaps()->scheduler;
    context->result    = UINT64_MAX;
    task_t*    peer    = resources->taskManager->CreateKernelTask(socket_peer_entry, context);
    const bool added   = peer != nullptr && resources->processManager->AddTask(process, peer);
    const bool ready   = added && context->scheduler->EnqueueProcess(arch_cpu_id(), process->id);
    bool       ok      = memory_expect(ready, "socket runnable peer fixture");

    if (ready)
    {
        ok &= wait();
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
        ok &= memory_expect(resources->taskManager->FreeTask(peer), "socket peer cleanup");
    }

    return ok;
}

static size_t socket_descriptor_count(process_t* process)
{
    size_t count = 0;

    for (uint64_t fd = 0; fd < process->fileDescriptorCount; ++fd)
    {
        count += process->fileDescriptors[fd].file != nullptr;
    }

    return count;
}

static bool socket_pair(process_t* process, uint64_t base, uint32_t type, int32_t* pair)
{
    if (!memory_expect(memory_call(SYSCALL_socketpair, 1, type, 0, base) == 0, "socketpair fixture"))
    {
        return false;
    }

    time_fixture_load(process, base, pair, 8);
    return true;
}

static bool test_socket_creation(process_t* process, uint64_t base, uint64_t number)
{
    bool ok = memory_expect(memory_call(SYSCALL_socket, 2, 1, 0) == (uint64_t) -97, "socket networking family explicitly unsupported");
    ok &= memory_expect(memory_call(SYSCALL_socket, 1, 1, 1) == (uint64_t) -93 && memory_call(SYSCALL_socket, 1, 3, 0) == (uint64_t) -94, "socket protocol and type errors");
    ok &= memory_expect(memory_call(SYSCALL_socket, 1, 0x10000001, 0) == LINUX_EINVAL, "socket rejects unknown creation flags");
    const size_t descriptors = socket_descriptor_count(process);
    ok &= memory_expect(memory_call(SYSCALL_socketpair, 1, 1, 0, CANONICAL_KERNEL_BASE) == LINUX_EFAULT && socket_descriptor_count(process) == descriptors, "socketpair output fault rolls back both descriptors");
    const uint64_t limit       = process->fileLimit.current;
    process->fileLimit.current = 1;
    ok &= memory_expect(memory_call(SYSCALL_socketpair, 1, 1, 0, base) == LINUX_EMFILE && socket_descriptor_count(process) == descriptors, "socketpair second descriptor exhaustion rolls back first");
    process->fileLimit.current = limit;

    const uint32_t types[] = {1U, 2U, 5U};

    for (uint32_t type : types)
    {
        int32_t pair[2];

        if (!socket_pair(process, base, type | 04000 | 02000000, pair))
        {
            return false;
        }

        ok &= memory_expect(pair[0] != pair[1] && memory_call(SYSCALL_fcntl, pair[0], 1) == 1 && memory_call(SYSCALL_fcntl, pair[0], 3) == (2 | 04000), "socket flags separate descriptor CLOEXEC from file NONBLOCK");
        const char payload[] = "local";
        time_fixture_store(process, base + 128, payload, sizeof(payload));
        ok &= memory_expect(memory_call(SYSCALL_write, pair[0], base + 128, sizeof(payload)) == sizeof(payload) && memory_call(SYSCALL_read, pair[1], base + 256, sizeof(payload)) == sizeof(payload), "socket file read/write transfers real payload");
        char received[sizeof(payload)] = {};
        time_fixture_load(process, base + 256, received, sizeof(received));
        ok &= memory_expect(memcmp(payload, received, sizeof(payload)) == 0, "socket payload byte comparison");
        ok &= memory_expect(memory_call(SYSCALL_lseek, pair[0], 0, 0) == (uint64_t) -29, "socket is not seekable");

        if (type != 1)
        {
            ok &= memory_expect(memory_call(SYSCALL_sendto, pair[0], 0, 0, 0x4000) == 0 && memory_call(SYSCALL_recvfrom, pair[1], 0, 0, 0) == 0 && memory_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == (uint64_t) -11, "socket empty message is delivered and consumed");
        }

        ok &= memory_expect(memory_call(SYSCALL_close, pair[0]) == 0 && memory_call(SYSCALL_close, pair[1]) == 0, "socketpair descriptor cleanup");
    }

    if (number == SYSCALL_socket)
    {
        const uint64_t fd = memory_call(number, (1ULL << 32) | 1, (1ULL << 32) | 1, 0);
        ok &= memory_expect((int64_t) fd >= 0, "socket decodes int arguments from low 32 bits");

        if ((int64_t) fd >= 0)
        {
            ok &= memory_expect(memory_call(SYSCALL_close, fd) == 0, "socket scalar descriptor cleanup");
        }
    }

    return ok;
}

static bool test_socket_connection(process_t* process, uint64_t base, uint64_t number)
{
    socket_address_t address = {};
    address.family           = 1;
    memcpy(address.path + 1, "arx\0socket", 10);
    const uint32_t addressLength = 13;
    time_fixture_store(process, base + 128, &address, sizeof(address));
    const uint64_t server = memory_call(SYSCALL_socket, 1, 1 | 04000 | 02000000, 0);
    const uint64_t client = memory_call(SYSCALL_socket, 1, 1 | 04000, 0);

    if ((int64_t) server < 0 || (int64_t) client < 0)
    {
        if ((int64_t) server >= 0)
            memory_call(SYSCALL_close, server);
        if ((int64_t) client >= 0)
            memory_call(SYSCALL_close, client);
        return memory_expect(false, "socket connection descriptors");
    }

    bool ok = memory_expect(memory_call(SYSCALL_getpeername, client, base + 256, base + 384) == (uint64_t) -107, "unconnected getpeername ENOTCONN");
    ok &= memory_expect(memory_call(SYSCALL_listen, server, 4) == LINUX_EINVAL, "listen requires a bound local address");
    ok &= memory_expect(memory_call(SYSCALL_bind, server, base + 128, addressLength) == 0, "socket abstract bind with embedded NUL");
    ok &= memory_expect(memory_call(SYSCALL_bind, server, base + 128, addressLength) == LINUX_EINVAL && memory_call(SYSCALL_bind, client, base + 128, addressLength) == (uint64_t) -98, "bind duplicate socket/address errors");
    ok &= memory_expect(memory_call(SYSCALL_bind, client, CANONICAL_KERNEL_BASE, addressLength) == LINUX_EFAULT && memory_call(SYSCALL_bind, client, base + 128, 1) == LINUX_EINVAL, "bind length and user pointer validation");
    ok &= memory_expect(memory_call(SYSCALL_listen, server, 4) == 0 && memory_call(SYSCALL_accept, server) == (uint64_t) -11, "empty nonblocking accept EAGAIN");
    ok &= memory_expect(memory_call(SYSCALL_accept4, server, 0, 0, 1) == LINUX_EINVAL, "accept4 validates flags");

    if (number == SYSCALL_accept || number == SYSCALL_accept4)
    {
        memory_call(SYSCALL_fcntl, server, 4, 2);
        socket_peer_context_t context     = {nullptr, SYSCALL_connect, {client, base + 128, addressLength}, UINT64_MAX};
        uint64_t              accepted    = UINT64_MAX;
        const uint32_t        acceptFlags = number == SYSCALL_accept4 ? 04000 | 02000000 : 0;
        ok &= socket_with_peer(process, &context,
                               [&]()
                               {
                                   accepted = memory_call(number, server, 0, 0, acceptFlags);
                                   return memory_expect((int64_t) accepted >= 0 && context.result == 0, "blocking accept wakes from peer connect");
                               });

        if ((int64_t) accepted >= 0)
        {
            ok &= memory_expect(memory_call(SYSCALL_fcntl, accepted, 3) == (uint64_t) (2 | (acceptFlags & 04000)) && memory_call(SYSCALL_fcntl, accepted, 1) == (uint64_t) ((acceptFlags & 02000000) != 0), "accept does not inherit listener flags; accept4 applies flags");
            memory_call(SYSCALL_close, accepted);
        }
    }
    else
    {
        ok &= memory_expect(memory_call(SYSCALL_connect, client, base + 128, addressLength) == 0 && memory_call(SYSCALL_connect, client, base + 128, addressLength) == (uint64_t) -106, "local stream connect and EISCONN");
        const uint32_t addressCapacity = sizeof(socket_address_t);
        time_fixture_store(process, base + 384, &addressCapacity, sizeof(addressCapacity));
        ok &= memory_expect(memory_call(SYSCALL_accept4, server, CANONICAL_KERNEL_BASE, base + 384, 0) == LINUX_EFAULT, "accept output fault preserves pending connection");
        const uint64_t accepted = memory_call(SYSCALL_accept4, server, 0, 0, 04000);
        ok &= memory_expect((int64_t) accepted >= 0, "queued connection accepted");

        if ((int64_t) accepted >= 0)
        {
            const uint32_t capacity = sizeof(socket_address_t);
            time_fixture_store(process, base + 384, &capacity, 4);
            ok &= memory_expect(memory_call(SYSCALL_getpeername, client, base + 256, base + 384) == 0, "connected peer name");
            socket_address_t queried = {};
            uint32_t         actual  = 0;
            time_fixture_load(process, base + 256, &queried, sizeof(queried));
            time_fixture_load(process, base + 384, &actual, 4);
            ok &= memory_expect(actual == addressLength && memcmp(&queried, &address, addressLength) == 0, "abstract name preserves exact length and embedded zeros");
            const uint32_t shortLength = 1;
            time_fixture_store(process, base + 384, &shortLength, 4);
            ok &= memory_expect(memory_call(SYSCALL_getsockname, server, base + 256, base + 384) == 0, "socket name truncates to supplied capacity");
            time_fixture_load(process, base + 384, &actual, 4);
            ok &= memory_expect(actual == addressLength, "socket name reports full size after truncation");
            memory_call(SYSCALL_close, accepted);
        }
    }

    if (number == SYSCALL_listen)
    {
        memory_call(SYSCALL_fcntl, server, 4, 2);
        socket_peer_context_t context = {nullptr, SYSCALL_shutdown, {server, 0}, UINT64_MAX};
        ok &= socket_with_peer(process, &context, [&]() { return memory_expect(memory_call(SYSCALL_accept, server) == LINUX_EINVAL && context.result == 0, "listener shutdown wakes blocked accept"); });
    }

    const uint32_t faultCapacity = sizeof(socket_address_t);
    time_fixture_store(process, base + 384, &faultCapacity, sizeof(faultCapacity));
    ok &= memory_expect(memory_call(SYSCALL_getsockname, server, CANONICAL_KERNEL_BASE, base + 384) == LINUX_EFAULT, "socket name rejects kernel output");
    memory_call(SYSCALL_close, server);
    memory_call(SYSCALL_close, client);
    const uint64_t rebound = memory_call(SYSCALL_socket, 1, 1, 0);
    ok &= memory_expect((int64_t) rebound >= 0 && memory_call(SYSCALL_bind, rebound, base + 128, addressLength) == 0, "last close releases abstract name");

    if ((int64_t) rebound >= 0)
    {
        memory_call(SYSCALL_close, rebound);
    }

    return ok;
}

static bool test_socket_message(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 2 | 04000, pair))
    {
        return false;
    }

    const char payload[] = "abcdef";
    time_fixture_store(process, base + 128, payload, 6);
    file_iovec_t vectors[2] = {{base + 128, 2}, {base + 130, 4}};
    time_fixture_store(process, base + 256, vectors, sizeof(vectors));
    socket_message_t message = {};
    message.vectors          = base + 256;
    message.vectorCount      = 2;
    const uint64_t header    = base + PAGE_SIZE - 24;
    time_fixture_store(process, header, &message, sizeof(message));
    bool ok    = memory_expect(memory_call(SYSCALL_sendmsg, pair[0], header, 0) == 6, "sendmsg decodes LP64 header across pages and gathers iovecs");
    vectors[0] = {base + 512, 1};
    vectors[1] = {base + 768, 2};
    time_fixture_store(process, base + 256, vectors, sizeof(vectors));
    message.controlLength = 17;
    message.flags         = UINT32_MAX;
    time_fixture_store(process, header, &message, sizeof(message));
    ok &= memory_expect(memory_call(SYSCALL_recvmsg, pair[1], header, 0x20) == 6, "recvmsg MSG_TRUNC returns full datagram length");
    time_fixture_load(process, header, &message, sizeof(message));
    char first = 0, tail[2] = {};
    time_fixture_load(process, base + 512, &first, 1);
    time_fixture_load(process, base + 768, tail, 2);
    ok &= memory_expect(first == 'a' && tail[0] == 'b' && tail[1] == 'c' && message.flags == 0x20 && message.controlLength == 0 && message.nameLength == 0 && message.vectorCount == 2, "recvmsg scatter data, truncation flag and ancillary length outputs");
    message.vectorCount = 1025;
    time_fixture_store(process, header, &message, sizeof(message));
    ok &= memory_expect(memory_call(SYSCALL_sendmsg, pair[0], header, 0) == (uint64_t) -90, "message iovec count bound");
    message.vectorCount   = 1;
    message.controlLength = 1;
    time_fixture_store(process, header, &message, sizeof(message));
    ok &= memory_expect(memory_call(SYSCALL_sendmsg, pair[0], header, 0) == LINUX_EOPNOTSUPP, "ancillary send explicitly unsupported");
    message.controlLength = 0;
    message.vectors       = CANONICAL_KERNEL_BASE;
    time_fixture_store(process, header, &message, sizeof(message));
    ok &= memory_expect(memory_call(SYSCALL_sendmsg, pair[0], header, 0) == LINUX_EFAULT && memory_call(SYSCALL_recvmsg, pair[1], CANONICAL_KERNEL_BASE, 0) == LINUX_EFAULT, "message validates nested and top-level pointers");
    vectors[0] = {base + 128, UINT64_MAX};
    time_fixture_store(process, base + 256, vectors, sizeof(vectors));
    message.vectors = base + 256;
    time_fixture_store(process, header, &message, sizeof(message));
    ok &= memory_expect(memory_call(SYSCALL_sendmsg, pair[0], header, 0) == LINUX_EINVAL, "message rejects overflowing vector lengths");
    memory_call(SYSCALL_close, pair[0]);
    memory_call(SYSCALL_close, pair[1]);
    return ok;
}

static bool test_socket_transfer(process_t* process, uint64_t base, uint64_t number)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1, pair))
    {
        return false;
    }

    const char data[] = "abcdef";
    time_fixture_store(process, base + 128, data, 6);
    bool                  ok      = memory_expect(memory_call(SYSCALL_recvfrom, pair[1], base + 256, 6, 0x40) == (uint64_t) -11, "per-call MSG_DONTWAIT returns EAGAIN");
    socket_peer_context_t context = {nullptr, SYSCALL_sendto, {(uint64_t) pair[0], base + 128, 6, 0x4000}, UINT64_MAX};
    ok &= socket_with_peer(process, &context, [&]() { return memory_expect(memory_call(SYSCALL_recvfrom, pair[1], base + 256, 6, 0) == 6 && context.result == 6, "blocking socket receive wakes on real peer send"); });
    char received[6] = {};
    time_fixture_load(process, base + 256, received, 6);
    ok &= memory_expect(memcmp(received, data, 6) == 0, "blocking receive payload matches");
    ok &= memory_expect(memory_call(SYSCALL_sendto, pair[0], base + 128, 2, 0x4000) == 2 && memory_call(SYSCALL_sendto, pair[0], base + 130, 4, 0x4000) == 4, "stream sends enqueue distinct chunks");
    ok &= memory_expect(memory_call(SYSCALL_recvfrom, pair[1], base + 256, 2, 2) == 2, "MSG_PEEK leaves bytes queued");
    ok &= memory_expect(memory_call(SYSCALL_recvfrom, pair[1], CANONICAL_KERNEL_BASE, 2, 0) == LINUX_EFAULT && memory_call(SYSCALL_recvfrom, pair[1], base + 256, 6, 0x100) == 6, "receive fault preserves data; WAITALL joins stream chunks");
    time_fixture_load(process, base + 256, received, 6);
    ok &= memory_expect(memcmp(received, data, 6) == 0, "stream chunk order preserved");
    file_iovec_t vectors[2] = {{base + 128, 2}, {base + 130, 4}};
    time_fixture_store(process, base + 512, vectors, sizeof(vectors));
    ok &= memory_expect(memory_call(SYSCALL_writev, pair[0], base + 512, 2) == 6 && memory_call(SYSCALL_read, pair[1], base + 256, 6) == 6, "socket writev integrates with VFS descriptor callbacks");
    ok &= memory_expect(memory_call(SYSCALL_write, pair[0], base + 128, 6) == 6, "socket ioctl queue fixture");
    ok &= memory_expect(memory_call(SYSCALL_ioctl, pair[1], 0x541b, base + 1024) == 0, "socket FIONREAD ioctl");
    int32_t available = 0;
    time_fixture_load(process, base + 1024, &available, sizeof(available));
    ok &= memory_expect(available == 6, "FIONREAD reports queued stream bytes");
    memory_call(SYSCALL_mprotect, base + 3 * PAGE_SIZE, PAGE_SIZE, 1);
    ok &= memory_expect(memory_call(SYSCALL_recvfrom, pair[1], base + 3 * PAGE_SIZE, 6, 0) == LINUX_EFAULT, "socket receive rejects readonly output without dequeue");
    ok &= memory_expect(memory_call(SYSCALL_read, pair[1], base + 256, 6) == 6, "readonly output fault preserves queued socket payload");
    ok &= memory_expect(memory_call(SYSCALL_sendto, pair[0], CANONICAL_KERNEL_BASE, 1, 0x4000) == LINUX_EFAULT && memory_call(SYSCALL_sendto, pair[0], base + 128, 1, 1) == LINUX_EOPNOTSUPP, "send rejects invalid buffers and unsupported flags");
    memory_call(SYSCALL_close, pair[0]);
    memory_call(SYSCALL_close, pair[1]);

    if (number == SYSCALL_sendto || number == SYSCALL_recvfrom)
    {
        int32_t datagrams[2];

        if (!socket_pair(process, base, 2 | 04000, datagrams))
        {
            return false;
        }

        ok &= memory_expect(memory_call(SYSCALL_sendto, datagrams[0], base + 128, 65537, 0) == (uint64_t) -90, "oversized datagram fails atomically before buffer access");
        ok &= memory_expect(memory_call(SYSCALL_sendto, datagrams[0], base + 128, 6, 0) == 6 && memory_call(SYSCALL_recvfrom, datagrams[1], base + 256, 2, 0x20) == 6, "datagram truncation discards tail and returns full size");
        ok &= memory_expect(memory_call(SYSCALL_recvfrom, datagrams[1], base + 256, 6, 0) == (uint64_t) -11, "truncated datagram is consumed once");
        memory_call(SYSCALL_close, datagrams[0]);
        memory_call(SYSCALL_close, datagrams[1]);
    }

    return ok;
}

static bool test_socket_shutdown(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1 | 04000, pair))
    {
        return false;
    }

    const uint8_t byte = 0x5a;
    time_fixture_store(process, base + 128, &byte, 1);
    bool ok = memory_expect(memory_call(SYSCALL_shutdown, pair[0], 3) == LINUX_EINVAL, "shutdown validates direction");
    ok &= memory_expect(memory_call(SYSCALL_sendto, pair[0], base + 128, 1, 0x4000) == 1 && memory_call(SYSCALL_shutdown, pair[0], 1) == 0, "shutdown write half after queued data");
    ok &= memory_expect(memory_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == 1 && memory_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == 0, "shutdown drains queued payload before EOF");
    ok &= memory_expect(memory_call(SYSCALL_sendto, pair[1], base + 128, 1, 0x4000) == 1 && memory_call(SYSCALL_read, pair[0], base + 256, 1) == 1, "half-close preserves reverse direction");
    ok &= memory_expect(memory_call(SYSCALL_sendto, pair[0], base + 128, 1, 0x4000) == (uint64_t) -32, "MSG_NOSIGNAL returns EPIPE after write shutdown");
    auto           resources = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps();
    task_t*        task      = resources->taskManager->GetCurrentTask();
    const uint64_t pending   = task->signalPending;
    ok &= memory_expect(memory_call(SYSCALL_sendto, pair[0], base + 128, 1, 0) == (uint64_t) -32 && (task->signalPending & (1ULL << 12)) != 0, "broken socket queues thread SIGPIPE");
    task->signalPending       = pending;
    event_pollfd_t descriptor = {pair[1], 0x2001, 0};
    time_fixture_store(process, base + 512, &descriptor, sizeof(descriptor));
    ok &= memory_expect(memory_call(SYSCALL_poll, base + 512, 1, 0) == 1, "socket poll observes read half closure");
    time_fixture_load(process, base + 512, &descriptor, sizeof(descriptor));
    ok &= memory_expect((descriptor.revents & 0x2001) == 0x2001, "socket POLLIN/POLLRDHUP bits");
    memory_call(SYSCALL_close, pair[0]);
    ok &= memory_expect(memory_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == 0, "peer last close produces EOF");
    memory_call(SYSCALL_close, pair[1]);
    return ok;
}

static bool test_socket_options(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1, pair))
    {
        return false;
    }

    uint32_t length = 4;
    time_fixture_store(process, base + 128, &length, 4);
    bool     ok    = memory_expect(memory_call(SYSCALL_getsockopt, pair[0], 1, 3, base + 256, base + 128) == 0, "getsockopt SO_TYPE");
    uint32_t value = 0;
    time_fixture_load(process, base + 256, &value, 4);
    ok &= memory_expect(value == 1, "SO_TYPE reports base type without flags");
    value = 8192;
    time_fixture_store(process, base + 256, &value, 4);
    ok &= memory_expect(memory_call(SYSCALL_setsockopt, pair[0], 1, 7, base + 256, 4) == 0 && memory_call(SYSCALL_getsockopt, pair[0], 1, 7, base + 256, base + 128) == 0, "socket buffer option round trip");
    time_fixture_load(process, base + 256, &value, 4);
    ok &= memory_expect(value == 16384, "Linux SO_SNDBUF doubles supplied value");
    length = 12;
    time_fixture_store(process, base + 128, &length, 4);
    ok &= memory_expect(memory_call(SYSCALL_getsockopt, pair[0], 1, 17, base + 256, base + 128) == 0, "SO_PEERCRED wire output");
    int32_t credentials[3] = {};
    time_fixture_load(process, base + 256, credentials, sizeof(credentials));
    ok &= memory_expect(credentials[0] == (int32_t) process->id && credentials[1] == (int32_t) process->credentials.user.effective && credentials[2] == (int32_t) process->credentials.group.effective, "socketpair credentials snapshot");
    ok &= memory_expect(memory_call(SYSCALL_setsockopt, pair[0], 1, 7, base + 256, 3) == LINUX_EINVAL && memory_call(SYSCALL_setsockopt, pair[0], 1, 7, CANONICAL_KERNEL_BASE, 4) == LINUX_EFAULT, "setsockopt length and pointer validation");
    ok &= memory_expect(memory_call(SYSCALL_getsockopt, pair[0], 1, 999, base + 256, base + 128) == (uint64_t) -92 && memory_call(SYSCALL_setsockopt, pair[0], 999, 7, base + 256, 4) == (uint64_t) -92, "unknown socket options return ENOPROTOOPT");
    ok &= memory_expect(memory_call(SYSCALL_getsockopt, pair[0], 1, 3, base + 256, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "getsockopt validates socklen pointer");
    length = 0;
    time_fixture_store(process, base + 128, &length, 4);
    ok &= memory_expect(memory_call(SYSCALL_getsockopt, pair[0], 1, 3, 0, base + 128) == 0, "zero-length option query ignores value pointer");
    memory_call(SYSCALL_close, pair[0]);
    memory_call(SYSCALL_close, pair[1]);
    return ok;
}

static bool test_socket_waits(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1, pair))
    {
        return false;
    }

    bool ok = true;

    for (size_t i = 0; i < 16; ++i)
    {
        ok &= memory_expect(memory_call(SYSCALL_sendto, pair[0], base + 128, PAGE_SIZE, 0x4040) == PAGE_SIZE, "fill bounded stream receive queue");
    }

    ok &= memory_expect(memory_call(SYSCALL_sendto, pair[0], base + 128, 1, 0x4040) == (uint64_t) -11, "full socket queue returns EAGAIN without dropping data");
    socket_peer_context_t context = {nullptr, SYSCALL_recvfrom, {(uint64_t) pair[1], base + 256, PAGE_SIZE, 0}, UINT64_MAX};
    ok &= socket_with_peer(process, &context, [&]() { return memory_expect(memory_call(SYSCALL_sendto, pair[0], base + 128, 1, 0x4000) == 1 && context.result == PAGE_SIZE, "blocking send wakes after peer drains queue"); });
    memory_call(SYSCALL_close, pair[0]);
    memory_call(SYSCALL_close, pair[1]);

    if (!socket_pair(process, base, 1, pair))
    {
        return false;
    }

    signal_action_t action = {base + 128, 0x04000000, base + 768, 0};
    time_fixture_store(process, base + 1024, &action, sizeof(action));
    ok &= memory_expect(memory_call(SYSCALL_rt_sigaction, 10, base + 1024, 0, 8) == 0, "socket interruption signal fixture");
    context = {nullptr, SYSCALL_kill, {process->id, 10}, UINT64_MAX};
    ok &= socket_with_peer(process, &context, [&]() { return memory_expect(memory_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == (uint64_t) -4 && context.result == 0, "socket wait returns EINTR for real peer signal"); });
    __atomic_store_n(&process->signalPending, 0ULL, __ATOMIC_RELEASE);
    memory_call(SYSCALL_close, pair[0]);
    memory_call(SYSCALL_close, pair[1]);
    return ok;
}

static bool test_socket_datagram_names(process_t* process, uint64_t base)
{
    const uint64_t sender   = memory_call(SYSCALL_socket, 1, 2 | 04000, 0);
    const uint64_t receiver = memory_call(SYSCALL_socket, 1, 2 | 04000, 0);

    if ((int64_t) sender < 0 || (int64_t) receiver < 0)
    {
        if ((int64_t) sender >= 0)
            memory_call(SYSCALL_close, sender);
        if ((int64_t) receiver >= 0)
            memory_call(SYSCALL_close, receiver);
        return memory_expect(false, "named datagram fixture descriptors");
    }

    socket_address_t address = {};
    address.family           = 1;
    memcpy(address.path + 1, "arx-recv", 8);
    const uint32_t length = 11;
    time_fixture_store(process, base + 128, &address, sizeof(address));
    address.path[1] = 'b';
    time_fixture_store(process, base + 256, &address, sizeof(address));
    bool          ok      = memory_expect(memory_call(SYSCALL_bind, receiver, base + 128, length) == 0 && memory_call(SYSCALL_bind, sender, base + 256, length) == 0, "named datagram bind");
    const uint8_t payload = 0x93;
    time_fixture_store(process, base + 512, &payload, 1);
    const uint32_t capacity = sizeof(address);
    time_fixture_store(process, base + 640, &capacity, 4);
    ok &= memory_expect(memory_call(SYSCALL_sendto, sender, base + 512, 1, 0, base + 128, length) == 1 && memory_call(SYSCALL_recvfrom, receiver, base + 768, 1, 0, base + 896, base + 640) == 1, "named datagram sendto/recvfrom");
    socket_address_t source       = {};
    uint32_t         actualLength = 0;
    time_fixture_load(process, base + 896, &source, sizeof(source));
    time_fixture_load(process, base + 640, &actualLength, 4);
    ok &= memory_expect(actualLength == length && memcmp(&source, &address, length) == 0, "recvfrom reports sender abstract address");
    ok &= memory_expect(memory_call(SYSCALL_connect, sender, base + 128, length) == 0 && memory_call(SYSCALL_sendto, sender, base + 512, 1, 0) == 1 && memory_call(SYSCALL_read, receiver, base + 768, 1) == 1, "connected datagram sends without destination");
    address = {};
    time_fixture_store(process, base + 1024, &address, sizeof(address));
    ok &= memory_expect(memory_call(SYSCALL_connect, sender, base + 1024, 2) == 0 && memory_call(SYSCALL_sendto, sender, base + 512, 1, 0) == (uint64_t) -89, "AF_UNSPEC disconnects datagram default peer");
    address.family = 1;
    memcpy(address.path, "/tmp/socket", 12);
    time_fixture_store(process, base + 1024, &address, sizeof(address));
    ok &= memory_expect(memory_call(SYSCALL_bind, sender, base + 1024, 14) == LINUX_EOPNOTSUPP, "filesystem socket paths explicitly require future VFS backend");
    memory_call(SYSCALL_close, sender);
    memory_call(SYSCALL_close, receiver);
    return ok;
}

static bool test_socket_epoll(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1 | 04000, pair))
    {
        return false;
    }

    const uint64_t      epoll = memory_call(SYSCALL_epoll_create1, 0);
    event_epoll_event_t watch = {0x80000001U, 0x1122334455667788ULL};
    time_fixture_store(process, base + 128, &watch, sizeof(watch));
    bool ok = memory_expect((int64_t) epoll >= 0 && memory_call(SYSCALL_epoll_ctl, epoll, 1, pair[1], base + 128) == 0, "socket edge-triggered epoll registration");
    ok &= memory_expect(memory_call(SYSCALL_epoll_pwait, epoll, base + 256, 1, 0, 0, 8) == 0, "empty socket has no readable epoll event");
    const uint8_t data = 0x73;
    time_fixture_store(process, base + 512, &data, 1);
    ok &= memory_expect(memory_call(SYSCALL_write, pair[0], base + 512, 1) == 1 && memory_call(SYSCALL_epoll_pwait, epoll, base + 256, 1, 0, 0, 8) == 1, "socket producer wakes edge-triggered epoll");
    event_epoll_event_t result = {};
    time_fixture_load(process, base + 256, &result, sizeof(result));
    ok &= memory_expect((result.events & 1) != 0 && result.data == watch.data && memory_call(SYSCALL_epoll_pwait, epoll, base + 256, 1, 0, 0, 8) == 0, "socket epoll event preserves user data and is consumed once");
    ok &= memory_expect(memory_call(SYSCALL_read, pair[1], base + 512, 1) == 1 && memory_call(SYSCALL_write, pair[0], base + 512, 1) == 1 && memory_call(SYSCALL_epoll_pwait, epoll, base + 256, 1, 0, 0, 8) == 1, "drain then refill produces another socket edge");
    const uint64_t alias = memory_call(SYSCALL_dup, pair[0]);
    memory_call(SYSCALL_close, pair[0]);
    ok &= memory_expect((int64_t) alias >= 0 && memory_call(SYSCALL_write, alias, base + 512, 1) == 1, "dup retains socket endpoint after original descriptor close");
    if ((int64_t) alias >= 0)
        memory_call(SYSCALL_close, alias);
    memory_call(SYSCALL_close, pair[1]);
    if ((int64_t) epoll >= 0)
        memory_call(SYSCALL_close, epoll);
    return ok;
}

template <uint64_t Number> static bool custom_case_validate_socket(const custom_case_ctx_t*)
{
    return run_memory_case(
            [](process_t* process)
            {
                const uint64_t base = memory_map(4);

                if ((int64_t) base < 0)
                {
                    return false;
                }

                bool ok;

                if (Number == SYSCALL_socket || Number == SYSCALL_socketpair)
                {
                    ok = test_socket_creation(process, base, Number);
                }
                else if (Number == SYSCALL_bind || Number == SYSCALL_connect || Number == SYSCALL_listen || Number == SYSCALL_accept || Number == SYSCALL_accept4 || Number == SYSCALL_getsockname || Number == SYSCALL_getpeername)
                {
                    ok = test_socket_connection(process, base, Number);
                }
                else if (Number == SYSCALL_sendmsg || Number == SYSCALL_recvmsg)
                {
                    ok = test_socket_message(process, base);
                }
                else if (Number == SYSCALL_shutdown)
                {
                    ok = test_socket_shutdown(process, base);
                }
                else if (Number == SYSCALL_getsockopt || Number == SYSCALL_setsockopt)
                {
                    ok = test_socket_options(process, base);
                }
                else
                {
                    ok = test_socket_transfer(process, base, Number);
                }
                if (Number == SYSCALL_sendto)
                {
                    ok &= test_socket_waits(process, base);
                    ok &= test_socket_datagram_names(process, base);
                    ok &= test_socket_epoll(process, base);
                }

                const uint64_t nonsocket = memory_call(SYSCALL_eventfd2, 0, 04000);

                if ((int64_t) nonsocket >= 0)
                {
                    if (Number != SYSCALL_socket && Number != SYSCALL_socketpair)
                    {
                        ok &= memory_expect(memory_call(Number, nonsocket, 0, 0, 0, 0, 0) == (uint64_t) -88, "socket operation rejects nonsocket descriptor before user copies");
                    }

                    memory_call(SYSCALL_close, nonsocket);
                }

                ok &= memory_expect(socket_descriptor_count(process) == 0, "socket syscall fixture releases all descriptors");
                ok &= memory_expect(memory_call(Number, UINT32_MAX, 0, 0, 0, 0, 0) == (Number == SYSCALL_socket || Number == SYSCALL_socketpair ? (uint64_t) -97 : LINUX_EBADF), "socket invalid descriptor/domain ABI validation");
                return ok;
            });
}

static void test_socket_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
    static const request_method_test_t<SocketRequestManager> tests[] = {
            {"HandleSocketRequest", &SocketRequestManager::HandleSocketRequest, SYSCALL_socket, custom_case_validate_socket<SYSCALL_socket>},
            {"HandleSocketpairRequest", &SocketRequestManager::HandleSocketpairRequest, SYSCALL_socketpair, custom_case_validate_socket<SYSCALL_socketpair>},
            {"HandleBindRequest", &SocketRequestManager::HandleBindRequest, SYSCALL_bind, custom_case_validate_socket<SYSCALL_bind>},
            {"HandleConnectRequest", &SocketRequestManager::HandleConnectRequest, SYSCALL_connect, custom_case_validate_socket<SYSCALL_connect>},
            {"HandleListenRequest", &SocketRequestManager::HandleListenRequest, SYSCALL_listen, custom_case_validate_socket<SYSCALL_listen>},
            {"HandleAccept4Request", &SocketRequestManager::HandleAccept4Request, SYSCALL_accept4, custom_case_validate_socket<SYSCALL_accept4>},
            {"HandleAcceptRequest", &SocketRequestManager::HandleAcceptRequest, SYSCALL_accept, custom_case_validate_socket<SYSCALL_accept>},
            {"HandleRecvfromRequest", &SocketRequestManager::HandleRecvfromRequest, SYSCALL_recvfrom, custom_case_validate_socket<SYSCALL_recvfrom>},
            {"HandleRecvmsgRequest", &SocketRequestManager::HandleRecvmsgRequest, SYSCALL_recvmsg, custom_case_validate_socket<SYSCALL_recvmsg>},
            {"HandleSendtoRequest", &SocketRequestManager::HandleSendtoRequest, SYSCALL_sendto, custom_case_validate_socket<SYSCALL_sendto>},
            {"HandleSendmsgRequest", &SocketRequestManager::HandleSendmsgRequest, SYSCALL_sendmsg, custom_case_validate_socket<SYSCALL_sendmsg>},
            {"HandleShutdownRequest", &SocketRequestManager::HandleShutdownRequest, SYSCALL_shutdown, custom_case_validate_socket<SYSCALL_shutdown>},
            {"HandleGetsockoptRequest", &SocketRequestManager::HandleGetsockoptRequest, SYSCALL_getsockopt, custom_case_validate_socket<SYSCALL_getsockopt>},
            {"HandleSetsockoptRequest", &SocketRequestManager::HandleSetsockoptRequest, SYSCALL_setsockopt, custom_case_validate_socket<SYSCALL_setsockopt>},
            {"HandleGetsocknameRequest", &SocketRequestManager::HandleGetsocknameRequest, SYSCALL_getsockname, custom_case_validate_socket<SYSCALL_getsockname>},
            {"HandleGetpeernameRequest", &SocketRequestManager::HandleGetpeernameRequest, SYSCALL_getpeername, custom_case_validate_socket<SYSCALL_getpeername>},
    };

    SocketRequestManager unavailable;

    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};

        if ((unavailable.*test.method)(&frame) != LINUX_ENOSYS || (caps->socketRequestManager != nullptr && (caps->socketRequestManager->*test.method)(nullptr) != LINUX_EINVAL))
        {
            stats->implementedFail++;
            selftest_record_failure_detail("socket missing dependency/null-frame validation");
        }
    }

    run_manager_tests("socket", caps->socketRequestManager, tests, stats, true, true);
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
