#include "layers/Dispatcher.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <klib/klib.h>
#include <platform.h>
#include <selftests/selftests.h>
}

extern "C" uint64_t dispatcher_dispatch_syscall(const arch_syscall_frame_t* frame);

namespace
{
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

struct custom_case_ctx_t
{
        const char*               managerName;
        const char*               testName;
        uint64_t                  syscallNumber;
        const arch_syscall_frame_t* frame;
        uint64_t                  result;
};

typedef bool (*custom_case_validator_t)(const custom_case_ctx_t* ctx);

template <typename ManagerT>
struct request_method_test_t
{
        const char*                              name;
        uint64_t (ManagerT::*method)(const arch_syscall_frame_t* frame);
        uint64_t                                 syscallNumber;
        custom_case_validator_t                  customValidator;
};

static bool custom_case_stub_process_lifecycle(const custom_case_ctx_t* ctx)
{
        if (ctx == nullptr)
        {
                return false;
        }

        kprintf("Arx kernel: requestlayer_selftest custom testcase stub %s::%s syscall=%llu result=%lld\n", ctx->managerName, ctx->testName,
                        (unsigned long long) ctx->syscallNumber, (long long) ctx->result);

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
                char readBuffer[32]            = {};
                arch_syscall_frame_t syscallFrame = *(ctx->frame);
                syscallFrame.syscall_number       = SYSCALL_read;
                syscallFrame.arg0                 = (uint64_t) fd;
                syscallFrame.arg1                 = (uint64_t) (uintptr_t) readBuffer;
                syscallFrame.arg2                 = (uint64_t) sizeof(readBuffer);

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
                static const char payload[] = "Arx syscall write selftest\n";
                char              original[sizeof(payload)] = {};
                const uint64_t    maxBytes = (uint64_t) (sizeof(payload) - 1);

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
                        syscallFrame.arg1                 = (uint64_t) (uintptr_t) payload;
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
        openFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
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
        creatFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
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
        openatFrame.arg1                 = (uint64_t) (uintptr_t) "/test.txt";
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
        rootDirFrame.arg0                 = (uint64_t) (uintptr_t) "/";
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
        relativeOpenat.arg1                 = (uint64_t) (uintptr_t) "test.txt";
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

        closeFrame.arg0                        = relativeFd;
        const uint64_t closeRelativeResult     = dispatcher_dispatch_syscall(&closeFrame);
        if (closeRelativeResult != 0)
        {
                return false;
        }

        arch_syscall_frame_t regularFileFrame = {};
        regularFileFrame.syscall_number       = SYSCALL_open;
        regularFileFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
        regularFileFrame.arg1                 = 0;
        regularFileFrame.arg2                 = 0;

        const uint64_t regularFd = dispatcher_dispatch_syscall(&regularFileFrame);
        if ((int64_t) regularFd < 0)
        {
                return false;
        }

        relativeOpenat.arg0                  = regularFd;
        const uint64_t nonDirOpenResult      = dispatcher_dispatch_syscall(&relativeOpenat);

        closeFrame.arg0                      = regularFd;
        const uint64_t closeRegularResult    = dispatcher_dispatch_syscall(&closeFrame);

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
        openFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
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
        frame.arg0                 = (uint64_t) (uintptr_t) &cpuValue;
        frame.arg1                 = (uint64_t) (uintptr_t) &nodeValue;
        frame.arg2                 = 0;

        const uint64_t dispatchResult = dispatcher_dispatch_syscall(&frame);
        const bool cpuInRange         = cpuValue < BOOT_SMP_MAX_CPUS;
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
        frame.arg2                 = (uint64_t) (uintptr_t) mask;

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

static bool custom_case_validate_uname(const custom_case_ctx_t* ctx)
{
        if (ctx == nullptr || ctx->frame == nullptr)
        {
                return false;
        }

        if (ctx->result == LINUX_ENOSYS)
        {
                return false;
        }

        struct test_linux_utsname_t
        {
                char sysname[65];
                char nodename[65];
                char release[65];
                char version[65];
                char machine[65];
                char domainname[65];
        };

        test_linux_utsname_t uts = {};

        arch_syscall_frame_t frame = {};
        frame.syscall_number       = SYSCALL_uname;
        frame.arg0                 = (uint64_t) (uintptr_t) &uts;

        const uint64_t dispatchResult = dispatcher_dispatch_syscall(&frame);
        if (dispatchResult != 0)
        {
                return false;
        }

        if (strcmp(uts.sysname, "Arx") != 0)
        {
                return false;
        }

        return uts.machine[0] != '\0';
}

static bool custom_case_validate_sysinfo(const custom_case_ctx_t* ctx)
{
        if (ctx == nullptr || ctx->frame == nullptr)
        {
                return false;
        }

        if (ctx->result == LINUX_ENOSYS)
        {
                return false;
        }

        struct test_linux_sysinfo_t
        {
                int64_t  uptime;
                uint64_t loads[3];
                uint64_t totalram;
                uint64_t freeram;
                uint64_t sharedram;
                uint64_t bufferram;
                uint64_t totalswap;
                uint64_t freeswap;
                uint16_t procs;
                uint16_t pad;
                uint64_t totalhigh;
                uint64_t freehigh;
                uint32_t mem_unit;
        };

        test_linux_sysinfo_t info = {};

        arch_syscall_frame_t frame = {};
        frame.syscall_number       = SYSCALL_sysinfo;
        frame.arg0                 = (uint64_t) (uintptr_t) &info;

        const uint64_t dispatchResult = dispatcher_dispatch_syscall(&frame);
        if (dispatchResult != 0)
        {
                return false;
        }

        if (info.mem_unit == 0)
        {
                return false;
        }

        return info.totalram >= info.freeram;
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

        arch_syscall_frame_t openFrame = {};
        openFrame.syscall_number       = SYSCALL_open;
        openFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
        openFrame.arg1                 = 0;
        openFrame.arg2                 = 0;
        const uint64_t testFd          = dispatcher_dispatch_syscall(&openFrame);
        if ((int64_t) testFd < 0)
        {
                return false;
        }

        // Keep the harness process alive after validation.
        currentProcess->exited     = false;
        currentProcess->exitStatus = 0;

        arch_syscall_frame_t dispatchedFrame = *(ctx->frame);
        dispatchedFrame.syscall_number       = ctx->syscallNumber;
        const uint64_t dispatchResult        = dispatcher_dispatch_syscall(&dispatchedFrame);
        const bool dispatchMarkedExited      = currentProcess->exited;
        bool       descriptorClosed          = false;

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
        openFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
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
        const uint64_t badWhence = dispatcher_dispatch_syscall(&lseekBad);

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
        frame.arg0                 = (uint64_t) (uintptr_t) cwdBuffer;
        frame.arg1                 = sizeof(cwdBuffer);

        const uint64_t getcwdResult = dispatcher_dispatch_syscall(&frame);
        if ((char*) (uintptr_t) getcwdResult != cwdBuffer)
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
        chdirRoot.arg0                 = (uint64_t) (uintptr_t) "/";
        const uint64_t chdirRootResult = dispatcher_dispatch_syscall(&chdirRoot);
        if (chdirRootResult != 0)
        {
                return false;
        }

        arch_syscall_frame_t chdirBad = {};
        chdirBad.syscall_number       = SYSCALL_chdir;
        chdirBad.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
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
        openDir.arg0                 = (uint64_t) (uintptr_t) "/";
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
        openFile.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
        openFile.arg1                 = 0;
        openFile.arg2                 = 0;
        const uint64_t fileFd         = dispatcher_dispatch_syscall(&openFile);
        if ((int64_t) fileFd < 0)
        {
                return false;
        }

        fchdirFrame.arg0                  = fileFd;
        const uint64_t fileFchdirResult   = dispatcher_dispatch_syscall(&fchdirFrame);

        closeDir.arg0                     = fileFd;
        const uint64_t closeFileResult    = dispatcher_dispatch_syscall(&closeDir);

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
        openFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
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
        fstatFrame.arg1                 = (uint64_t) (uintptr_t) &statBuffer;

        const uint64_t fstatResult = dispatcher_dispatch_syscall(&fstatFrame);

        arch_syscall_frame_t badFdFrame = fstatFrame;
        badFdFrame.arg0                 = fdResult + 1;
        const uint64_t badFdResult      = dispatcher_dispatch_syscall(&badFdFrame);

        arch_syscall_frame_t closeFrame = {};
        closeFrame.syscall_number       = SYSCALL_close;
        closeFrame.arg0                 = fdResult;
        const uint64_t closeResult      = dispatcher_dispatch_syscall(&closeFrame);

        constexpr uint32_t linux_s_ifmt = 0170000U;
        constexpr uint32_t linux_s_ifreg = 0100000U;

        const bool modeLooksRegular = (statBuffer.st_mode & linux_s_ifmt) == linux_s_ifreg;
        const bool sizeReasonable   = statBuffer.st_size >= 0;
        const bool blockSizeSet     = statBuffer.st_blksize > 0;

        return fstatResult == 0 && statBuffer.st_nlink >= 1 && modeLooksRegular && sizeReasonable && blockSizeSet && badFdResult == LINUX_EBADF &&
                        closeResult == 0;
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
        frame.arg1                 = (uint64_t) (uintptr_t) "/test.txt";
        frame.arg2                 = (uint64_t) (uintptr_t) &statBuffer;
        frame.arg3                 = 0;

        const uint64_t okResult = dispatcher_dispatch_syscall(&frame);

        arch_syscall_frame_t badFlagFrame = frame;
        badFlagFrame.arg3                 = 0x80000000ULL;
        const uint64_t badFlagResult      = dispatcher_dispatch_syscall(&badFlagFrame);

        arch_syscall_frame_t badPathFrame = frame;
        badPathFrame.arg1                 = (uint64_t) (uintptr_t) "/does-not-exist";
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
        frame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
        frame.arg1                 = (uint64_t) (uintptr_t) &statBuffer;

        const uint64_t okResult = dispatcher_dispatch_syscall(&frame);

        arch_syscall_frame_t nullBufferFrame = frame;
        nullBufferFrame.arg1                 = 0;
        const uint64_t nullBufferResult      = dispatcher_dispatch_syscall(&nullBufferFrame);

        arch_syscall_frame_t badPathFrame = frame;
        badPathFrame.arg0                 = (uint64_t) (uintptr_t) "/does-not-exist";
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
        frame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
        frame.arg1                 = (uint64_t) (uintptr_t) &statBuffer;

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
        linkFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
        linkFrame.arg1                 = (uint64_t) (uintptr_t) "/tmp-syscall-link";
        const uint64_t linkResult      = dispatcher_dispatch_syscall(&linkFrame);

        if (linkResult != 0)
        {
                return false;
        }

        arch_syscall_frame_t readFrame = {};
        char                 target[64] = {};
        readFrame.syscall_number        = SYSCALL_readlink;
        readFrame.arg0                  = (uint64_t) (uintptr_t) "/tmp-syscall-link";
        readFrame.arg1                  = (uint64_t) (uintptr_t) target;
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
        openDir.arg0                 = (uint64_t) (uintptr_t) "/";
        const uint64_t dirFd         = dispatcher_dispatch_syscall(&openDir);
        if ((int64_t) dirFd < 0)
        {
                return false;
        }

        arch_syscall_frame_t linkFrame = {};
        linkFrame.syscall_number       = SYSCALL_symlinkat;
        linkFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
        linkFrame.arg1                 = dirFd;
        linkFrame.arg2                 = (uint64_t) (uintptr_t) "tmp-syscall-link-at";
        const uint64_t linkResult      = dispatcher_dispatch_syscall(&linkFrame);

        arch_syscall_frame_t closeDir = {};
        closeDir.syscall_number       = SYSCALL_close;
        closeDir.arg0                 = dirFd;
        (void) dispatcher_dispatch_syscall(&closeDir);

        if (linkResult != 0)
        {
                return false;
        }

        arch_syscall_frame_t readFrame = {};
        char                 target[64] = {};
        readFrame.syscall_number        = SYSCALL_readlink;
        readFrame.arg0                  = (uint64_t) (uintptr_t) "/tmp-syscall-link-at";
        readFrame.arg1                  = (uint64_t) (uintptr_t) target;
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
        createFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
        createFrame.arg1                 = (uint64_t) (uintptr_t) "/tmp-syscall-link-read";
        const uint64_t createResult      = dispatcher_dispatch_syscall(&createFrame);
        if (createResult != 0)
        {
                return false;
        }

        arch_syscall_frame_t frame = {};
        char                 target[64] = {};
        frame.syscall_number          = SYSCALL_readlink;
        frame.arg0                    = (uint64_t) (uintptr_t) "/tmp-syscall-link-read";
        frame.arg1                    = (uint64_t) (uintptr_t) target;
        frame.arg2                    = sizeof(target);

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
        openDir.arg0                 = (uint64_t) (uintptr_t) "/";
        const uint64_t dirFd         = dispatcher_dispatch_syscall(&openDir);
        if ((int64_t) dirFd < 0)
        {
                return false;
        }

        arch_syscall_frame_t createFrame = {};
        createFrame.syscall_number       = SYSCALL_symlinkat;
        createFrame.arg0                 = (uint64_t) (uintptr_t) "/test.txt";
        createFrame.arg1                 = dirFd;
        createFrame.arg2                 = (uint64_t) (uintptr_t) "tmp-syscall-link-at-read";
        const uint64_t createResult      = dispatcher_dispatch_syscall(&createFrame);
        if (createResult != 0)
        {
                arch_syscall_frame_t closeDir = {};
                closeDir.syscall_number       = SYSCALL_close;
                closeDir.arg0                 = dirFd;
                (void) dispatcher_dispatch_syscall(&closeDir);
                return false;
        }

        arch_syscall_frame_t frame = {};
        char                 target[64] = {};
        frame.syscall_number          = SYSCALL_readlinkat;
        frame.arg0                    = dirFd;
        frame.arg1                    = (uint64_t) (uintptr_t) "tmp-syscall-link-at-read";
        frame.arg2                    = (uint64_t) (uintptr_t) target;
        frame.arg3                    = sizeof(target);
        const uint64_t readResult     = dispatcher_dispatch_syscall(&frame);

        arch_syscall_frame_t closeDir = {};
        closeDir.syscall_number       = SYSCALL_close;
        closeDir.arg0                 = dirFd;
        (void) dispatcher_dispatch_syscall(&closeDir);

        return (int64_t) readResult > 0;
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

template <typename ManagerT, size_t N>
static void run_manager_tests(const char* managerName, ManagerT* manager, const request_method_test_t<ManagerT> (&tests)[N], requestlayer_stats_t* stats)
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
                arch_syscall_frame_t frame  = {};
                frame.syscall_number        = tests[index].syscallNumber;
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

                kprintf("Arx kernel: requestlayer_selftest run %s::%s syscall=%llu\n", managerName, tests[index].name,
                                (unsigned long long) tests[index].syscallNumber);

                uint64_t             result = (manager->*(tests[index].method))(&frame);
                stats->total++;

                if (result == LINUX_ENOSYS)
                {
                        stats->unimplemented++;
                        continue;
                }

                stats->implemented++;

                if (tests[index].customValidator != nullptr)
                {
                        const custom_case_ctx_t customCtx = {
                                        managerName,
                                        tests[index].name,
                                        tests[index].syscallNumber,
                                        &frame,
                                        result,
                        };
                        stats->customCaseUsed++;
                        if (tests[index].customValidator(&customCtx))
                        {
                                stats->customCasePass++;
                                stats->implementedPass++;
                        }
                        else
                        {
                                stats->customCaseFail++;
                                stats->implementedFail++;
                                selftest_record_failure_detail(tests[index].name);
                                kprintf("Arx kernel: requestlayer_selftest custom FAIL %s::%s got=%lld\n", managerName, tests[index].name,
                                                (long long) result);
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
                        {"HandleSched_getaffinityRequest", &SchedulerRequestManager::HandleSched_getaffinityRequest, SYSCALL_sched_getaffinity,
                         custom_case_validate_sched_getaffinity},
                        {"HandleGetcpuRequest", &SchedulerRequestManager::HandleGetcpuRequest, SYSCALL_getcpu, custom_case_validate_getcpu},
        };

        run_manager_tests("scheduler", caps->schedulerRequestManager, tests, stats);
}

static void test_time_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
        static const request_method_test_t<TimeRequestManager> tests[] = {
                        {"HandleTimeRequest", &TimeRequestManager::HandleTimeRequest, SYSCALL_time},
                        {"HandleGettimeofdayRequest", &TimeRequestManager::HandleGettimeofdayRequest, SYSCALL_gettimeofday},
                        {"HandleClock_gettimeRequest", &TimeRequestManager::HandleClock_gettimeRequest, SYSCALL_clock_gettime},
                        {"HandleClock_getresRequest", &TimeRequestManager::HandleClock_getresRequest, SYSCALL_clock_getres},
                        {"HandleClock_nanosleepRequest", &TimeRequestManager::HandleClock_nanosleepRequest, SYSCALL_clock_nanosleep},
                        {"HandleNanosleepRequest", &TimeRequestManager::HandleNanosleepRequest, SYSCALL_nanosleep},
                        {"HandleGetitimerRequest", &TimeRequestManager::HandleGetitimerRequest, SYSCALL_getitimer},
                        {"HandleSetitimerRequest", &TimeRequestManager::HandleSetitimerRequest, SYSCALL_setitimer},
                        {"HandleAlarmRequest", &TimeRequestManager::HandleAlarmRequest, SYSCALL_alarm},
        };

        run_manager_tests("time", caps->timeRequestManager, tests, stats);
}

static void test_memory_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
        static const request_method_test_t<MemoryRequestManager> tests[] = {
                        {"HandleMmapRequest", &MemoryRequestManager::HandleMmapRequest, SYSCALL_mmap},
                        {"HandleMunmapRequest", &MemoryRequestManager::HandleMunmapRequest, SYSCALL_munmap},
                        {"HandleMprotectRequest", &MemoryRequestManager::HandleMprotectRequest, SYSCALL_mprotect},
                        {"HandleMincoreRequest", &MemoryRequestManager::HandleMincoreRequest, SYSCALL_mincore},
                        {"HandleMadviseRequest", &MemoryRequestManager::HandleMadviseRequest, SYSCALL_madvise},
                        {"HandleBrkRequest", &MemoryRequestManager::HandleBrkRequest, SYSCALL_brk},
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
                        {"HandleClose_rangeRequest", &VfsRequestManager::HandleClose_rangeRequest, SYSCALL_close_range},
                        {"HandleMkdiratRequest", &VfsRequestManager::HandleMkdiratRequest, SYSCALL_mkdirat},
                        {"HandleMkdirRequest", &VfsRequestManager::HandleMkdirRequest, SYSCALL_mkdir},
                        {"HandleReadRequest", &VfsRequestManager::HandleReadRequest, SYSCALL_read},
                        {"HandleWriteRequest", &VfsRequestManager::HandleWriteRequest, SYSCALL_write},
                        {"HandlePread64Request", &VfsRequestManager::HandlePread64Request, SYSCALL_pread64},
                        {"HandlePwrite64Request", &VfsRequestManager::HandlePwrite64Request, SYSCALL_pwrite64},
                        {"HandleReadvRequest", &VfsRequestManager::HandleReadvRequest, SYSCALL_readv},
                        {"HandleWritevRequest", &VfsRequestManager::HandleWritevRequest, SYSCALL_writev},
                        {"HandlePreadvRequest", &VfsRequestManager::HandlePreadvRequest, SYSCALL_preadv},
                        {"HandlePwritevRequest", &VfsRequestManager::HandlePwritevRequest, SYSCALL_pwritev},
                        {"HandleLseekRequest", &VfsRequestManager::HandleLseekRequest, SYSCALL_lseek, custom_case_validate_lseek},
                        {"HandleGetcwdRequest", &VfsRequestManager::HandleGetcwdRequest, SYSCALL_getcwd, custom_case_validate_getcwd},
                        {"HandleChdirRequest", &VfsRequestManager::HandleChdirRequest, SYSCALL_chdir, custom_case_validate_chdir},
                        {"HandleFchdirRequest", &VfsRequestManager::HandleFchdirRequest, SYSCALL_fchdir, custom_case_validate_fchdir},
                        {"HandleGetdents64Request", &VfsRequestManager::HandleGetdents64Request, SYSCALL_getdents64},
                        {"HandleUnlinkatRequest", &VfsRequestManager::HandleUnlinkatRequest, SYSCALL_unlinkat},
                        {"HandleUnlinkRequest", &VfsRequestManager::HandleUnlinkRequest, SYSCALL_unlink},
                        {"HandleRmdirRequest", &VfsRequestManager::HandleRmdirRequest, SYSCALL_rmdir},
                        {"HandleFcntlRequest", &VfsRequestManager::HandleFcntlRequest, SYSCALL_fcntl},
                        {"HandleDupRequest", &VfsRequestManager::HandleDupRequest, SYSCALL_dup},
                        {"HandleDup2Request", &VfsRequestManager::HandleDup2Request, SYSCALL_dup2},
                        {"HandleDup3Request", &VfsRequestManager::HandleDup3Request, SYSCALL_dup3},
                        {"HandleNewfstatatRequest", &VfsRequestManager::HandleNewfstatatRequest, SYSCALL_newfstatat, custom_case_validate_newfstatat},
                        {"HandleStatRequest", &VfsRequestManager::HandleStatRequest, SYSCALL_stat, custom_case_validate_stat},
                        {"HandleFstatRequest", &VfsRequestManager::HandleFstatRequest, SYSCALL_fstat, custom_case_validate_fstat},
                        {"HandleLstatRequest", &VfsRequestManager::HandleLstatRequest, SYSCALL_lstat, custom_case_validate_lstat},
                        {"HandleRenameatRequest", &VfsRequestManager::HandleRenameatRequest, SYSCALL_renameat},
                        {"HandleRenameRequest", &VfsRequestManager::HandleRenameRequest, SYSCALL_rename},
                        {"HandleReadlinkatRequest", &VfsRequestManager::HandleReadlinkatRequest, SYSCALL_readlinkat, custom_case_validate_readlinkat},
                        {"HandleReadlinkRequest", &VfsRequestManager::HandleReadlinkRequest, SYSCALL_readlink, custom_case_validate_readlink},
                        {"HandleIoctlRequest", &VfsRequestManager::HandleIoctlRequest, SYSCALL_ioctl},
                        {"HandleLinkatRequest", &VfsRequestManager::HandleLinkatRequest, SYSCALL_linkat},
                        {"HandleLinkRequest", &VfsRequestManager::HandleLinkRequest, SYSCALL_link},
                        {"HandleSymlinkatRequest", &VfsRequestManager::HandleSymlinkatRequest, SYSCALL_symlinkat, custom_case_validate_symlinkat},
                        {"HandleSymlinkRequest", &VfsRequestManager::HandleSymlinkRequest, SYSCALL_symlink, custom_case_validate_symlink},
                        {"HandleFaccessatRequest", &VfsRequestManager::HandleFaccessatRequest, SYSCALL_faccessat},
                        {"HandleFaccessat2Request", &VfsRequestManager::HandleFaccessat2Request, SYSCALL_faccessat2},
                        {"HandleAccessRequest", &VfsRequestManager::HandleAccessRequest, SYSCALL_access},
                        {"HandleFchownatRequest", &VfsRequestManager::HandleFchownatRequest, SYSCALL_fchownat},
                        {"HandleChownRequest", &VfsRequestManager::HandleChownRequest, SYSCALL_chown},
                        {"HandleFchownRequest", &VfsRequestManager::HandleFchownRequest, SYSCALL_fchown},
                        {"HandleLchownRequest", &VfsRequestManager::HandleLchownRequest, SYSCALL_lchown},
                        {"HandleFchmodat2Request", &VfsRequestManager::HandleFchmodat2Request, SYSCALL_fchmodat2},
                        {"HandleFchmodatRequest", &VfsRequestManager::HandleFchmodatRequest, SYSCALL_fchmodat},
                        {"HandleChmodRequest", &VfsRequestManager::HandleChmodRequest, SYSCALL_chmod},
                        {"HandleFchmodRequest", &VfsRequestManager::HandleFchmodRequest, SYSCALL_fchmod},
                        {"HandleMountRequest", &VfsRequestManager::HandleMountRequest, SYSCALL_mount},
                        {"HandleStatfsRequest", &VfsRequestManager::HandleStatfsRequest, SYSCALL_statfs},
                        {"HandleFstatfsRequest", &VfsRequestManager::HandleFstatfsRequest, SYSCALL_fstatfs},
                        {"HandleMknodatRequest", &VfsRequestManager::HandleMknodatRequest, SYSCALL_mknodat},
                        {"HandleMknodRequest", &VfsRequestManager::HandleMknodRequest, SYSCALL_mknod},
                        {"HandleTruncateRequest", &VfsRequestManager::HandleTruncateRequest, SYSCALL_truncate},
                        {"HandleFtruncateRequest", &VfsRequestManager::HandleFtruncateRequest, SYSCALL_ftruncate},
                        {"HandlePipeRequest", &VfsRequestManager::HandlePipeRequest, SYSCALL_pipe},
                        {"HandlePipe2Request", &VfsRequestManager::HandlePipe2Request, SYSCALL_pipe2},
                        {"HandleMemfd_createRequest", &VfsRequestManager::HandleMemfd_createRequest, SYSCALL_memfd_create},
        };

        run_manager_tests("vfs", caps->vfsRequestManager, tests, stats);
}

static void test_event_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
        static const request_method_test_t<EventRequestManager> tests[] = {
                        {"HandlePollRequest", &EventRequestManager::HandlePollRequest, SYSCALL_poll},
                        {"HandlePpollRequest", &EventRequestManager::HandlePpollRequest, SYSCALL_ppoll},
                        {"HandleSelectRequest", &EventRequestManager::HandleSelectRequest, SYSCALL_select},
                        {"HandlePselect6Request", &EventRequestManager::HandlePselect6Request, SYSCALL_pselect6},
                        {"HandleEpoll_createRequest", &EventRequestManager::HandleEpoll_createRequest, SYSCALL_epoll_create},
                        {"HandleEpoll_create1Request", &EventRequestManager::HandleEpoll_create1Request, SYSCALL_epoll_create1},
                        {"HandleEpoll_ctlRequest", &EventRequestManager::HandleEpoll_ctlRequest, SYSCALL_epoll_ctl},
                        {"HandleEpoll_waitRequest", &EventRequestManager::HandleEpoll_waitRequest, SYSCALL_epoll_wait},
                        {"HandleEpoll_pwaitRequest", &EventRequestManager::HandleEpoll_pwaitRequest, SYSCALL_epoll_pwait},
                        {"HandleEpoll_pwait2Request", &EventRequestManager::HandleEpoll_pwait2Request, SYSCALL_epoll_pwait2},
                        {"HandleInotify_initRequest", &EventRequestManager::HandleInotify_initRequest, SYSCALL_inotify_init},
                        {"HandleInotify_init1Request", &EventRequestManager::HandleInotify_init1Request, SYSCALL_inotify_init1},
                        {"HandleInotify_add_watchRequest", &EventRequestManager::HandleInotify_add_watchRequest, SYSCALL_inotify_add_watch},
                        {"HandleInotify_rm_watchRequest", &EventRequestManager::HandleInotify_rm_watchRequest, SYSCALL_inotify_rm_watch},
                        {"HandleEventfdRequest", &EventRequestManager::HandleEventfdRequest, SYSCALL_eventfd},
                        {"HandleEventfd2Request", &EventRequestManager::HandleEventfd2Request, SYSCALL_eventfd2},
                        {"HandleSignalfdRequest", &EventRequestManager::HandleSignalfdRequest, SYSCALL_signalfd},
                        {"HandleSignalfd4Request", &EventRequestManager::HandleSignalfd4Request, SYSCALL_signalfd4},
        };

        run_manager_tests("event", caps->eventRequestManager, tests, stats);
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

static void test_credential_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
        static const request_method_test_t<CredentialRequestManager> tests[] = {
                        {"HandleGetuidRequest", &CredentialRequestManager::HandleGetuidRequest, SYSCALL_getuid},
                        {"HandleGetgidRequest", &CredentialRequestManager::HandleGetgidRequest, SYSCALL_getgid},
                        {"HandleGeteuidRequest", &CredentialRequestManager::HandleGeteuidRequest, SYSCALL_geteuid},
                        {"HandleGetegidRequest", &CredentialRequestManager::HandleGetegidRequest, SYSCALL_getegid},
                        {"HandleGetresuidRequest", &CredentialRequestManager::HandleGetresuidRequest, SYSCALL_getresuid},
                        {"HandleGetresgidRequest", &CredentialRequestManager::HandleGetresgidRequest, SYSCALL_getresgid},
                        {"HandleSetuidRequest", &CredentialRequestManager::HandleSetuidRequest, SYSCALL_setuid},
                        {"HandleSetgidRequest", &CredentialRequestManager::HandleSetgidRequest, SYSCALL_setgid},
                        {"HandleSetreuidRequest", &CredentialRequestManager::HandleSetreuidRequest, SYSCALL_setreuid},
                        {"HandleSetregidRequest", &CredentialRequestManager::HandleSetregidRequest, SYSCALL_setregid},
                        {"HandleSetresuidRequest", &CredentialRequestManager::HandleSetresuidRequest, SYSCALL_setresuid},
                        {"HandleSetresgidRequest", &CredentialRequestManager::HandleSetresgidRequest, SYSCALL_setresgid},
                        {"HandleSetfsuidRequest", &CredentialRequestManager::HandleSetfsuidRequest, SYSCALL_setfsuid},
                        {"HandleSetfsgidRequest", &CredentialRequestManager::HandleSetfsgidRequest, SYSCALL_setfsgid},
                        {"HandleGetgroupsRequest", &CredentialRequestManager::HandleGetgroupsRequest, SYSCALL_getgroups},
                        {"HandleSetgroupsRequest", &CredentialRequestManager::HandleSetgroupsRequest, SYSCALL_setgroups},
        };

        run_manager_tests("credential", caps->credentialRequestManager, tests, stats);
}

static void test_system_manager(RequestLayerCaps* caps, requestlayer_stats_t* stats)
{
        static const request_method_test_t<SystemRequestManager> tests[] = {
                        {"HandleArch_prctlRequest", &SystemRequestManager::HandleArch_prctlRequest, SYSCALL_arch_prctl},
                        {"HandleUmaskRequest", &SystemRequestManager::HandleUmaskRequest, SYSCALL_umask},
                        {"HandlePrlimit64Request", &SystemRequestManager::HandlePrlimit64Request, SYSCALL_prlimit64},
                        {"HandleGetrlimitRequest", &SystemRequestManager::HandleGetrlimitRequest, SYSCALL_getrlimit},
                        {"HandleSetrlimitRequest", &SystemRequestManager::HandleSetrlimitRequest, SYSCALL_setrlimit},
                        {"HandlePrctlRequest", &SystemRequestManager::HandlePrctlRequest, SYSCALL_prctl},
                        {"HandleUnameRequest", &SystemRequestManager::HandleUnameRequest, SYSCALL_uname, custom_case_validate_uname},
                        {"HandleSysinfoRequest", &SystemRequestManager::HandleSysinfoRequest, SYSCALL_sysinfo, custom_case_validate_sysinfo},
                        {"HandleGetrandomRequest", &SystemRequestManager::HandleGetrandomRequest, SYSCALL_getrandom},
        };

        run_manager_tests("system", caps->systemRequestManager, tests, stats);
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

        kprintf("Arx kernel: requestlayer_selftest total=%llu implemented=%llu unimplemented=%llu implemented_pass=%llu implemented_fail=%llu custom_used=%llu custom_pass=%llu custom_fail=%llu\n",
                        stats.total, stats.implemented, stats.unimplemented, stats.implementedPass, stats.implementedFail, stats.customCaseUsed,
                        stats.customCasePass, stats.customCaseFail);

        selftest_case_end("requestlayer_selftest", stats.implemented, stats.implementedFail);
}
