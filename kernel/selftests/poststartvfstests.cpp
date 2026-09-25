#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <klib/klib.h>
#include <platform.h>
#include <selftests/selftests.h>
}

struct poststart_vfs_process_test_context_t
{
    VirtualFileSystem* vfs;
    ProcessManager*    processManager;
    TaskManager*       taskManager;
    task_t*            bspTask;

    volatile int       taskReached;
    volatile int       taskPassed;
    volatile int       taskFailed;
    volatile int64_t   assignedFd;
};

static poststart_vfs_process_test_context_t g_poststart_vfs_process_ctx;

static void poststart_vfs_test_fail(const char* message, unsigned long long* failures)
{
    (*failures)++;
    kprintf("Arx kernel: poststart_vfs_selftest FAIL: %s\n", message);
}

static void poststart_vfs_process_file_task(void* arg)
{
    poststart_vfs_process_test_context_t* ctx = static_cast<poststart_vfs_process_test_context_t*>(arg);
    if (ctx == nullptr || ctx->vfs == nullptr || ctx->processManager == nullptr || ctx->taskManager == nullptr || ctx->bspTask == nullptr)
    {
        if (ctx != nullptr)
        {
            ctx->taskFailed = 1;
            ctx->taskReached = 1;
        }
        for (;;)
        {
            arch_pause();
        }
    }

    ctx->taskReached = 1;

    process_t* currentProcess = ctx->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    vfs_path_t start = {};
    file_t* opened = ctx->vfs->Open(start, "/test.txt", 0);
    if (opened == nullptr)
    {
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    const int64_t assignedFd = ctx->processManager->AddFileDescriptor(currentProcess, static_cast<file_handle_t>(opened), FD_FLAG_NONE);
    ctx->assignedFd = assignedFd;
    if (assignedFd < 0)
    {
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    char original[128] = {};
    const int64_t originalBytes = ctx->vfs->Read(opened, original, sizeof(original));
    if (originalBytes <= 0)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    static const char replacement[] = "Arx VFS writable test payload\n";
    const uint64_t replacementLen = (uint64_t) (sizeof(replacement) - 1);
    if ((uint64_t) originalBytes < replacementLen)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    if (ctx->vfs->Seek(opened, 0, 0) < 0)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    const int64_t writeBytes = ctx->vfs->Write(opened, replacement, replacementLen);
    if (writeBytes != (int64_t) replacementLen)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    if (ctx->vfs->Seek(opened, 0, 0) < 0)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    char verify[128] = {};
    const int64_t verifyBytes = ctx->vfs->Read(opened, verify, replacementLen);
    if (verifyBytes != (int64_t) replacementLen || memcmp(verify, replacement, replacementLen) != 0)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    if (ctx->vfs->Seek(opened, 0, 0) < 0)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    const int64_t restoreBytes = ctx->vfs->Write(opened, original, (uint64_t) originalBytes);
    if (restoreBytes != originalBytes)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    if (ctx->vfs->Seek(opened, 0, 0) < 0)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    char restored[128] = {};
    const int64_t restoredBytes = ctx->vfs->Read(opened, restored, (uint64_t) originalBytes);
    if (restoredBytes != originalBytes || memcmp(restored, original, (size_t) originalBytes) != 0)
    {
        currentProcess->fileDescriptors[assignedFd].file = nullptr;
        currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->taskFailed = 1;
        ctx->taskManager->ExecuteTask(ctx->bspTask);
        for (;;)
        {
            arch_pause();
        }
    }

    currentProcess->fileDescriptors[assignedFd].file = nullptr;
    currentProcess->fileDescriptors[assignedFd].flags = FD_FLAG_NONE;

    if (ctx->vfs->Close(opened) != 0)
    {
        ctx->taskFailed = 1;
    }
    else
    {
        ctx->taskPassed = 1;
    }

    ctx->taskManager->ExecuteTask(ctx->bspTask);
    for (;;)
    {
        arch_pause();
    }
}

extern "C" void run_poststart_vfs_selftests(void* resourceLayerCaps, void* logicLayerCaps)
{
    unsigned long long passes = 0;
    unsigned long long fails  = 0;
    VirtualFileSystem* vfs    = nullptr;
    ProcessManager* processManager = nullptr;
    TaskManager* taskManager = nullptr;
    Scheduler* scheduler = nullptr;
    process_t* process = nullptr;
    task_t* task = nullptr;
    virt_addr_space_t* currentSpace = nullptr;

    kprintf("Arx kernel: poststart_vfs_selftest start\n");

    ResourceLayerCaps* resourceCaps = static_cast<ResourceLayerCaps*>(resourceLayerCaps);
    LogicLayerCaps* logicCaps = static_cast<LogicLayerCaps*>(logicLayerCaps);
    if (resourceCaps == nullptr || resourceCaps->processManager == nullptr || resourceCaps->taskManager == nullptr)
    {
        poststart_vfs_test_fail("missing resource managers", &fails);
        goto done;
    }

    if (logicCaps == nullptr || logicCaps->virtualFileSystem == nullptr || logicCaps->scheduler == nullptr)
    {
        poststart_vfs_test_fail("missing virtual file system", &fails);
        goto done;
    }

    processManager = resourceCaps->processManager;
    taskManager = resourceCaps->taskManager;
    scheduler = logicCaps->scheduler;
    vfs = logicCaps->virtualFileSystem;

    {

    vfs_path_t start = {};
    vfs_path_t resolved = {};
    if (!vfs->ResolvePath(start, "/test.c", &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
    {
        poststart_vfs_test_fail("ResolvePath should find mounted initramfs file /test.c", &fails);
    }
    else
    {
        passes++;
    }

    file_t* opened = vfs->Open(start, "/test.c", 0);
    if (opened == nullptr)
    {
        poststart_vfs_test_fail("Open should open mounted initramfs file /test.c", &fails);
        goto done;
    }
    passes++;

    char buffer[16] = {};
    const int64_t readResult = vfs->Read(opened, buffer, sizeof(buffer) - 1);
    if (readResult <= 0)
    {
        poststart_vfs_test_fail("Read should return bytes from mounted initramfs file", &fails);
    }
    else
    {
        passes++;
    }

    if (readResult > 0)
    {
        buffer[(readResult < (int64_t)(sizeof(buffer) - 1)) ? readResult : (int64_t)(sizeof(buffer) - 1)] = '\0';
        if (readResult < 8 || memcmp(buffer, "#include", 8) != 0)
        {
            poststart_vfs_test_fail("Read should begin with expected initramfs test.c prefix", &fails);
        }
        else
        {
            passes++;
        }
    }

    if (vfs->Close(opened) != 0)
    {
        poststart_vfs_test_fail("Close should succeed for mounted initramfs file", &fails);
    }
    else
    {
        passes++;
    }

    }

    {
    memset(&g_poststart_vfs_process_ctx, 0, sizeof(g_poststart_vfs_process_ctx));
    g_poststart_vfs_process_ctx.vfs = vfs;
    g_poststart_vfs_process_ctx.processManager = processManager;
    g_poststart_vfs_process_ctx.taskManager = taskManager;
    g_poststart_vfs_process_ctx.bspTask = taskManager->GetCurrentTask();
    g_poststart_vfs_process_ctx.assignedFd = -1;

    if (g_poststart_vfs_process_ctx.bspTask == nullptr)
    {
        poststart_vfs_test_fail("missing BSP task for process VFS test", &fails);
        goto process_cleanup;
    }

    currentSpace = platform.cpus[arch_cpu_id()].address_space;
    if (currentSpace == nullptr)
    {
        poststart_vfs_test_fail("missing current address space for process VFS test", &fails);
        goto process_cleanup;
    }

    process = processManager->CreateProcess(currentSpace);
    if (process == nullptr)
    {
        poststart_vfs_test_fail("failed to create process for VFS process test", &fails);
        goto process_cleanup;
    }

    task = taskManager->CreateKernelTask(poststart_vfs_process_file_task, &g_poststart_vfs_process_ctx);
    if (task == nullptr)
    {
        poststart_vfs_test_fail("failed to create process VFS task", &fails);
        goto process_cleanup;
    }

    if (!processManager->AddTask(process, task))
    {
        poststart_vfs_test_fail("failed to bind VFS task to process", &fails);
        goto process_cleanup;
    }

    if (!scheduler->ScheduleProcess(process->id))
    {
        poststart_vfs_test_fail("failed to schedule process VFS task", &fails);
        goto process_cleanup;
    }

    if (!g_poststart_vfs_process_ctx.taskReached)
    {
        poststart_vfs_test_fail("process VFS task was not reached", &fails);
        goto process_cleanup;
    }

    if (g_poststart_vfs_process_ctx.taskFailed || !g_poststart_vfs_process_ctx.taskPassed)
    {
        poststart_vfs_test_fail("process VFS task failed read/write/restore sequence", &fails);
        goto process_cleanup;
    }

    passes++;

process_cleanup:
    if (task != nullptr)
    {
        taskManager->FreeTask(task);
    }

    if (process != nullptr)
    {
        processManager->FreeProcess(process);
    }
    }

done:
    kprintf("Arx kernel: poststart_vfs_selftest summary: pass=%llu fail=%llu\n", passes, fails);
    kprintf("Arx kernel: poststart_vfs_selftest RESULT=%s\n", fails == 0 ? "PASS" : "FAIL");
    if (fails == 0)
    {
        KDEBUG("poststart_vfs_selftest passed with %llu checks\n", passes);
    }
    else
    {
        KDEBUG("poststart_vfs_selftest failed with %llu checks\n", fails);
    }
}
