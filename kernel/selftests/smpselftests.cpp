#include "layers/Dispatcher.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TaskManager.hpp"

#include <arch/arch.h>
#include <klib/klib.h>
#include <platform.h>

typedef struct smp_pid_vfs_task_context
{
    VirtualFileSystem* vfs;
    ProcessManager*    processManager;
    TaskManager*       taskManager;
    task_t*            returnTask;
    uint64_t           cpuId;
    uint64_t           processId;
    volatile int       completed;
    volatile int       passed;
} smp_pid_vfs_task_context_t;

static size_t format_pid_payload(uint64_t pid, char* out, size_t outCapacity)
{
    if (out == nullptr || outCapacity < 6)
    {
        return 0;
    }

    out[0] = 'P';
    out[1] = 'I';
    out[2] = 'D';
    out[3] = '=';

    char   digits[21];
    size_t digitCount = 0;

    do
    {
        digits[digitCount++] = (char) ('0' + (pid % 10));
        pid /= 10;
    } while (pid != 0 && digitCount < sizeof(digits));

    if (4 + digitCount > outCapacity)
    {
        return 0;
    }

    for (size_t i = 0; i < digitCount; i++)
    {
        out[4 + i] = digits[digitCount - 1 - i];
    }

    return 4 + digitCount;
}

static void smp_pid_vfs_task(void* arg)
{
    smp_pid_vfs_task_context_t* ctx = (smp_pid_vfs_task_context_t*) arg;
    if (ctx == nullptr || ctx->vfs == nullptr || ctx->processManager == nullptr || ctx->taskManager == nullptr || ctx->returnTask == nullptr)
    {
        for (;;)
        {
            arch_pause();
        }
    }

    process_t* currentProcess = ctx->processManager->GetCurrentProcess();
    file_t*    opened         = nullptr;
    int64_t    fd             = -1;
    int        localFail      = 0;
    vfs_path_t start          = {};

    char probe[256];
    char payload[32];
    char verify[32];
    memset(probe, 0, sizeof(probe));
    memset(payload, 0, sizeof(payload));
    memset(verify, 0, sizeof(verify));

    if (currentProcess == nullptr)
    {
        localFail = 1;
    }

    if (!localFail)
    {
        opened = ctx->vfs->Open(start, "/test.txt", 0);
        if (opened == nullptr)
        {
            localFail = 1;
        }
    }

    if (!localFail)
    {
        fd = ctx->processManager->AddFileDescriptor(currentProcess, (file_handle_t) opened, FD_FLAG_NONE);
        if (fd < 0)
        {
            localFail = 1;
        }
    }

    int64_t fileBytes = -1;
    if (!localFail)
    {
        fileBytes = ctx->vfs->Read(opened, probe, sizeof(probe));
        if (fileBytes <= 0)
        {
            localFail = 1;
        }
    }

    size_t payloadLen = 0;
    if (!localFail)
    {
        payloadLen = format_pid_payload(ctx->processId, payload, sizeof(payload));
        if (payloadLen == 0)
        {
            localFail = 1;
        }
    }

    const uint64_t slotSize   = 16;
    const uint64_t slotOffset = ctx->cpuId * slotSize;
    if (!localFail && (slotOffset + (uint64_t) payloadLen > (uint64_t) fileBytes))
    {
        localFail = 1;
    }

    if (!localFail && ctx->vfs->Seek(opened, (int64_t) slotOffset, 0) < 0)
    {
        localFail = 1;
    }

    if (!localFail && ctx->vfs->Write(opened, payload, (uint64_t) payloadLen) != (int64_t) payloadLen)
    {
        localFail = 1;
    }

    if (!localFail && ctx->vfs->Seek(opened, (int64_t) slotOffset, 0) < 0)
    {
        localFail = 1;
    }

    if (!localFail)
    {
        const int64_t verifyBytes = ctx->vfs->Read(opened, verify, (uint64_t) payloadLen);
        if (verifyBytes != (int64_t) payloadLen || memcmp(verify, payload, payloadLen) != 0)
        {
            localFail = 1;
        }
    }

    if (fd >= 0 && currentProcess != nullptr && currentProcess->fileDescriptors != nullptr && (uint64_t) fd < currentProcess->fileDescriptorCount)
    {
        currentProcess->fileDescriptors[fd].file  = nullptr;
        currentProcess->fileDescriptors[fd].flags = FD_FLAG_NONE;
    }

    if (opened != nullptr)
    {
        if (ctx->vfs->Close(opened) != 0)
        {
            localFail = 1;
        }
    }

    ctx->passed    = localFail ? 0 : 1;
    ctx->completed = 1;

    (void) ctx->taskManager->ExecuteTask(ctx->returnTask);
    for (;;)
    {
        arch_pause();
    }
}

void smp_selftests(void)
{
    const uint64_t cpuId = (uint64_t) arch_cpu_id();

    Dispatcher* dispatcher = (Dispatcher*) platform.dispacher;
    if (dispatcher == nullptr)
    {
        kprintf("Arx kernel: smp pid-vfs test skipped cpu=%u (dispatcher null)\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs test skipped cpu=%u (dispatcher null)\n", (unsigned) cpuId);
        return;
    }

    ResourceLayerCaps* resourceLayerCaps = dispatcher->GetResourceLayerCaps();
    LogicLayerCaps*    logicLayerCaps    = dispatcher->GetLogicLayerCaps();
    if (resourceLayerCaps == nullptr || logicLayerCaps == nullptr || resourceLayerCaps->taskManager == nullptr || resourceLayerCaps->processManager == nullptr || logicLayerCaps->virtualFileSystem == nullptr)
    {
        kprintf("Arx kernel: smp pid-vfs test skipped cpu=%u (missing managers)\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs test skipped cpu=%u (missing managers)\n", (unsigned) cpuId);
        return;
    }

    TaskManager*       taskManager    = resourceLayerCaps->taskManager;
    ProcessManager*    processManager = resourceLayerCaps->processManager;
    VirtualFileSystem* vfs            = logicLayerCaps->virtualFileSystem;

    kprintf("Arx kernel: smp pid-vfs start cpu=%u\n", (unsigned) cpuId);
    KDEBUG("smp pid-vfs start cpu=%u\n", (unsigned) cpuId);

    unsigned long long passes = 0;
    unsigned long long fails  = 0;

    task_t*            returnTask = taskManager->GetRunningTask((uint8_t) cpuId);
    process_t*         originalRun = processManager->GetRunningProcess((uint8_t) cpuId);
    virt_addr_space_t* activeSpace = platform.cpus[cpuId].address_space;
    process_t*         process      = nullptr;
    task_t*            task         = nullptr;
    smp_pid_vfs_task_context_t ctx  = {};

    if (returnTask == nullptr)
    {
        fails++;
        kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u missing running task\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs FAIL cpu=%u missing running task\n", (unsigned) cpuId);
        goto smp_done;
    }

    if (activeSpace == nullptr && originalRun != nullptr)
    {
        activeSpace = originalRun->addressSpace;
    }

    if (activeSpace == nullptr && platform.bsp_id < platform.cpu_count)
    {
        activeSpace = platform.cpus[platform.bsp_id].address_space;
    }

    if (activeSpace == nullptr)
    {
        fails++;
        kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u missing address space\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs FAIL cpu=%u missing address space\n", (unsigned) cpuId);
        goto smp_done;
    }

    process = processManager->CreateProcess(activeSpace);
    if (process == nullptr)
    {
        fails++;
        kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u create process failed\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs FAIL cpu=%u create process failed\n", (unsigned) cpuId);
        goto smp_done;
    }

    ctx.vfs            = vfs;
    ctx.processManager = processManager;
    ctx.taskManager    = taskManager;
    ctx.returnTask     = returnTask;
    ctx.cpuId          = cpuId;
    ctx.processId      = process->id;
    ctx.completed      = 0;
    ctx.passed         = 0;

    task = taskManager->CreateKernelTask(smp_pid_vfs_task, &ctx);
    if (task == nullptr)
    {
        fails++;
        kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u create task failed\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs FAIL cpu=%u create task failed\n", (unsigned) cpuId);
        goto smp_done;
    }

    if (!processManager->AddTask(process, task))
    {
        fails++;
        kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u add task failed\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs FAIL cpu=%u add task failed\n", (unsigned) cpuId);
        goto smp_done;
    }

    if (!processManager->SetRunningProcess((uint8_t) cpuId, process))
    {
        fails++;
        kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u set running process failed\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs FAIL cpu=%u set running process failed\n", (unsigned) cpuId);
        goto smp_done;
    }

    if (!taskManager->ExecuteTask(task))
    {
        fails++;
        kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u execute task failed\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs FAIL cpu=%u execute task failed\n", (unsigned) cpuId);
        goto smp_done;
    }

    if (!processManager->SetRunningProcess((uint8_t) cpuId, originalRun))
    {
        fails++;
        kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u restore running process failed\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs FAIL cpu=%u restore running process failed\n", (unsigned) cpuId);
        goto smp_done;
    }

    if (ctx.completed != 1 || ctx.passed != 1)
    {
        fails++;
        kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u completed=%d passed=%d\n", (unsigned) cpuId, ctx.completed, ctx.passed);
        KDEBUG("smp pid-vfs FAIL cpu=%u completed=%d passed=%d\n", (unsigned) cpuId, ctx.completed, ctx.passed);
        goto smp_done;
    }

    passes++;
    kprintf("Arx kernel: smp pid-vfs PASS cpu=%u pid=%llu\n", (unsigned) cpuId, (unsigned long long) ctx.processId);
    KDEBUG("smp pid-vfs PASS cpu=%u pid=%llu\n", (unsigned) cpuId, (unsigned long long) ctx.processId);

smp_done:
    if (task != nullptr)
    {
        (void) taskManager->FreeTask(task);
    }

    if (process != nullptr)
    {
        (void) processManager->FreeProcess(process);
    }

    if (processManager->GetRunningProcess((uint8_t) cpuId) != originalRun)
    {
        (void) processManager->SetRunningProcess((uint8_t) cpuId, originalRun);
    }

    kprintf("Arx kernel: smp pid-vfs result cpu=%u pass=%llu fail=%llu\n", (unsigned) cpuId, passes, fails);
    KDEBUG("smp pid-vfs result cpu=%u pass=%llu fail=%llu\n", (unsigned) cpuId, passes, fails);
}
