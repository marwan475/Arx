#include "layers/Dispatcher.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TaskManager.hpp"

#include <arch/arch.h>
#include <klib/klib.h>
#include <platform.h>

extern "C"
{
#include <selftests/selftests.h>
}

typedef struct smp_vfs_rw_task_context
{
    VirtualFileSystem* vfs;
    ProcessManager*    processManager;
    TaskManager*       taskManager;
    task_t*            returnTask;

    volatile int reached;
    volatile int passed;
    volatile int failed;
} smp_vfs_rw_task_context_t;

static void smp_vfs_rw_task(void* arg)
{
    smp_vfs_rw_task_context_t* ctx = (smp_vfs_rw_task_context_t*) arg;
    if (ctx == 0 || ctx->vfs == 0 || ctx->processManager == 0 || ctx->taskManager == 0 || ctx->returnTask == 0)
    {
        if (ctx != 0)
        {
            ctx->failed  = 1;
            ctx->reached = 1;
        }
        for (;;)
        {
            arch_pause();
        }
    }

    ctx->reached = 1;

    process_t* currentProcess = ctx->processManager->GetCurrentProcess();
    if (currentProcess == 0)
    {
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    vfs_path_t start  = {};
    file_t*    opened = ctx->vfs->Open(start, "/test.txt", 0);
    if (opened == 0)
    {
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    const int64_t fd = ctx->processManager->AddFileDescriptor(currentProcess, (file_handle_t) opened, FD_FLAG_NONE);
    if (fd < 0)
    {
        (void) ctx->vfs->Close(opened);
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    char          original[128] = {};
    const int64_t originalBytes = ctx->vfs->Read(opened, original, sizeof(original));
    if (originalBytes <= 0)
    {
        currentProcess->fileDescriptors[fd].file  = 0;
        currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    static const char smpPayload[] = "Arx SMP VFS RW test payload\n";
    const uint64_t    payloadLen   = (uint64_t) (sizeof(smpPayload) - 1);
    if ((uint64_t) originalBytes < payloadLen)
    {
        currentProcess->fileDescriptors[fd].file  = 0;
        currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    if (ctx->vfs->Seek(opened, 0, 0) < 0)
    {
        currentProcess->fileDescriptors[fd].file  = 0;
        currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    const int64_t writeBytes = ctx->vfs->Write(opened, smpPayload, payloadLen);
    if (writeBytes != (int64_t) payloadLen)
    {
        currentProcess->fileDescriptors[fd].file  = 0;
        currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    if (ctx->vfs->Seek(opened, 0, 0) < 0)
    {
        currentProcess->fileDescriptors[fd].file  = 0;
        currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    char          verify[128] = {};
    const int64_t verifyBytes = ctx->vfs->Read(opened, verify, payloadLen);
    if (verifyBytes != (int64_t) payloadLen || memcmp(verify, smpPayload, payloadLen) != 0)
    {
        currentProcess->fileDescriptors[fd].file  = 0;
        currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    if (ctx->vfs->Seek(opened, 0, 0) < 0)
    {
        currentProcess->fileDescriptors[fd].file  = 0;
        currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    const int64_t restoreBytes = ctx->vfs->Write(opened, original, (uint64_t) originalBytes);
    if (restoreBytes != originalBytes)
    {
        currentProcess->fileDescriptors[fd].file  = 0;
        currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
        (void) ctx->vfs->Close(opened);
        ctx->failed = 1;
        ctx->taskManager->ExecuteTask(ctx->returnTask);
        for (;;)
        {
            arch_pause();
        }
    }

    currentProcess->fileDescriptors[fd].file  = 0;
    currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;

    if (ctx->vfs->Close(opened) != 0)
    {
        ctx->failed = 1;
    }
    else
    {
        ctx->passed = 1;
    }

    ctx->taskManager->ExecuteTask(ctx->returnTask);
    for (;;)
    {
        arch_pause();
    }
}

void smp_selftests(void)
{
    static spinlock_t       smp_selftests_lock         = 0;
    static uint8_t          smp_selftests_initialized  = 0;
    static volatile uint8_t smp_selftests_summary_done = 0;
    static size_t           smp_selftests_participants = 0;
    static size_t           smp_selftests_completed    = 0;
    static size_t           smp_selftests_passed       = 0;
    static size_t           smp_selftests_failed       = 0;

    const uint64_t cpu_id = (uint64_t) arch_cpu_id();

    Dispatcher* dispatcher = (Dispatcher*) platform.dispacher;
    if (dispatcher == nullptr)
    {
        kprintf("Arx kernel: cpu %u smp_selftests skipped (dispatcher null)\n", (unsigned) cpu_id);
        return;
    }

    ResourceLayerCaps* resourceLayerCaps = dispatcher->GetResourceLayerCaps();
    LogicLayerCaps*    logicLayerCaps    = dispatcher->GetLogicLayerCaps();
    if (resourceLayerCaps == nullptr || logicLayerCaps == nullptr || resourceLayerCaps->taskManager == nullptr || resourceLayerCaps->processManager == nullptr || logicLayerCaps->virtualFileSystem == nullptr)
    {
        kprintf("Arx kernel: cpu %u smp_selftests skipped (missing layer managers)\n", (unsigned) cpu_id);
        return;
    }

    TaskManager*       taskManager    = resourceLayerCaps->taskManager;
    ProcessManager*    processManager = resourceLayerCaps->processManager;
    VirtualFileSystem* vfs            = logicLayerCaps->virtualFileSystem;

    spinlock_acquire(&smp_selftests_lock);
    if (smp_selftests_initialized == 0)
    {
        smp_selftests_participants = 0;
        smp_selftests_completed    = 0;
        smp_selftests_summary_done = 0;
        smp_selftests_passed       = 0;
        smp_selftests_failed       = 0;
        selftest_reset_context();
        kprintf("Arx kernel: starting SMP subsystem selftests on %llu CPUs\n", (unsigned long long) platform.cpu_count);
        KDEBUG("smp_selftests start cpu_count=%llu\n", (unsigned long long) platform.cpu_count);
        smp_selftests_initialized = 1;
    }
    smp_selftests_participants++;
    spinlock_release(&smp_selftests_lock);

    unsigned long long        passes              = 0;
    unsigned long long        fails               = 0;
    task_t*                   returnTask          = taskManager->GetRunningTask((uint8_t) cpu_id);
    process_t*                originalRun         = processManager->GetRunningProcess((uint8_t) cpu_id);
    virt_addr_space_t*        activeSpace         = platform.cpus[cpu_id].address_space;
    process_t*                process             = 0;
    task_t*                   task                = 0;
    smp_vfs_rw_task_context_t ctx                 = {};

    selftest_case_begin("smp_process_task_vfs_rw");

    if (returnTask == 0)
    {
        fails++;
        kprintf("Arx kernel: smp_process_task_vfs_rw FAIL cpu=%u missing running task\n", (unsigned) cpu_id);
        goto smp_case_done;
    }

    if (activeSpace == 0)
    {
        if (originalRun != 0)
        {
            activeSpace = originalRun->addressSpace;
        }

        if (activeSpace == 0 && platform.bsp_id < platform.cpu_count)
        {
            activeSpace = platform.cpus[platform.bsp_id].address_space;
        }
    }

    if (activeSpace == 0)
    {
        fails++;
        kprintf("Arx kernel: smp_process_task_vfs_rw FAIL cpu=%u missing address space\n", (unsigned) cpu_id);
        goto smp_case_done;
    }

    process = processManager->CreateProcess(activeSpace);
    if (process == 0)
    {
        fails++;
        kprintf("Arx kernel: smp_process_task_vfs_rw FAIL cpu=%u create process failed\n", (unsigned) cpu_id);
        goto smp_case_done;
    }

    ctx.vfs            = vfs;
    ctx.processManager = processManager;
    ctx.taskManager    = taskManager;
    ctx.returnTask     = returnTask;
    ctx.reached        = 0;
    ctx.passed         = 0;
    ctx.failed         = 0;

    task = taskManager->CreateKernelTask(smp_vfs_rw_task, &ctx);
    if (task == 0)
    {
        fails++;
        kprintf("Arx kernel: smp_process_task_vfs_rw FAIL cpu=%u create task failed\n", (unsigned) cpu_id);
        goto smp_case_done;
    }

    if (!processManager->AddTask(process, task))
    {
        fails++;
        kprintf("Arx kernel: smp_process_task_vfs_rw FAIL cpu=%u add task to process failed\n", (unsigned) cpu_id);
        goto smp_case_done;
    }

    if (!processManager->SetRunningProcess((uint8_t) cpu_id, process))
    {
        fails++;
        kprintf("Arx kernel: smp_process_task_vfs_rw FAIL cpu=%u set running process failed\n", (unsigned) cpu_id);
        goto smp_case_done;
    }

    if (!taskManager->ExecuteTask(task))
    {
        fails++;
        kprintf("Arx kernel: smp_process_task_vfs_rw FAIL cpu=%u execute task failed\n", (unsigned) cpu_id);
        goto smp_case_done;
    }

    if (!processManager->SetRunningProcess((uint8_t) cpu_id, originalRun))
    {
        fails++;
        kprintf("Arx kernel: smp_process_task_vfs_rw FAIL cpu=%u restore running process failed\n", (unsigned) cpu_id);
        goto smp_case_done;
    }

    if (ctx.reached != 1 || ctx.passed != 1 || ctx.failed != 0)
    {
        fails++;
        kprintf("Arx kernel: smp_process_task_vfs_rw FAIL cpu=%u reached=%d passed=%d failed=%d\n", (unsigned) cpu_id, ctx.reached, ctx.passed, ctx.failed);
        goto smp_case_done;
    }

    passes++;
    kprintf("Arx kernel: smp_process_task_vfs_rw PASS cpu=%u\n", (unsigned) cpu_id);

smp_case_done:
    if (task != 0)
    {
        taskManager->FreeTask(task);
    }

    if (process != 0)
    {
        processManager->FreeProcess(process);
    }

    if (processManager->GetRunningProcess((uint8_t) cpu_id) != originalRun)
    {
        (void) processManager->SetRunningProcess((uint8_t) cpu_id, originalRun);
    }

    selftest_case_end("smp_process_task_vfs_rw", passes, fails);

    spinlock_acquire(&smp_selftests_lock);
    if (fails == 0)
    {
        smp_selftests_passed++;
    }
    else
    {
        smp_selftests_failed++;
    }
    smp_selftests_completed++;
    KDEBUG("smp_selftests cpu=%u result=%s participants=%llu completed=%llu\n", (unsigned) cpu_id, fails == 0 ? "PASS" : "FAIL", (unsigned long long) smp_selftests_participants, (unsigned long long) smp_selftests_completed);
    spinlock_release(&smp_selftests_lock);

    if (cpu_id == platform.bsp_id)
    {
        for (;;)
        {
            bool all_done = false;

            spinlock_acquire(&smp_selftests_lock);
            if (smp_selftests_participants == platform.cpu_count && smp_selftests_completed == platform.cpu_count)
            {
                all_done = true;
            }
            spinlock_release(&smp_selftests_lock);

            if (all_done)
            {
                break;
            }

            arch_pause();
        }

        selftest_print_summary();
        KDEBUG("smp_selftests summary participants=%llu completed=%llu passed=%llu failed=%llu\n", (unsigned long long) smp_selftests_participants, (unsigned long long) smp_selftests_completed, (unsigned long long) smp_selftests_passed, (unsigned long long) smp_selftests_failed);
        smp_selftests_summary_done = 1;
    }
    else
    {
        while (smp_selftests_summary_done == 0)
        {
            arch_pause();
        }
    }
}
