#include "layers/Dispatcher.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TaskManager.hpp"

#include <arch/arch.h>
#include <klib/klib.h>
#include <platform.h>
#include <selftests/selftests.h>

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
    volatile int       failStage;
} smp_pid_vfs_task_context_t;

typedef struct smp_test_totals
{
    unsigned long long passes;
    unsigned long long fails;
} smp_test_totals_t;

static volatile unsigned long long g_smp_selftests_total_passes   = 0;
static volatile unsigned long long g_smp_selftests_total_fails    = 0;
static volatile unsigned long long g_smp_selftests_finished_cpus  = 0;
static volatile unsigned long long g_smp_selftests_fail_mask      = 0;

enum smp_fail_mask_bits
{
    SMP_FAIL_DISPATCHER_NULL            = 1ULL << 0,
    SMP_FAIL_MANAGERS_MISSING           = 1ULL << 1,
    SMP_FAIL_PIDVFS_MISSING_RUNNING     = 1ULL << 2,
    SMP_FAIL_PIDVFS_MISSING_ADDR_SPACE  = 1ULL << 3,
    SMP_FAIL_PIDVFS_CREATE_PROCESS      = 1ULL << 4,
    SMP_FAIL_PIDVFS_CREATE_TASK         = 1ULL << 5,
    SMP_FAIL_PIDVFS_ADD_TASK            = 1ULL << 6,
    SMP_FAIL_PIDVFS_SET_RUNNING         = 1ULL << 7,
    SMP_FAIL_PIDVFS_EXECUTE             = 1ULL << 8,
    SMP_FAIL_PIDVFS_RESTORE_RUNNING     = 1ULL << 9,
    SMP_FAIL_PIDVFS_TASK_BODY           = 1ULL << 10,
    SMP_FAIL_PIDVFS_SLOT_LAYOUT         = 1ULL << 11,

    SMP_FAIL_SCHED_MISSING_MANAGERS     = 1ULL << 16,
    SMP_FAIL_SCHED_MISSING_RUNNING      = 1ULL << 17,
    SMP_FAIL_SCHED_MISSING_ADDR_SPACE   = 1ULL << 18,
    SMP_FAIL_SCHED_CREATE_PROCESS       = 1ULL << 19,
    SMP_FAIL_SCHED_CREATE_TASK          = 1ULL << 20,
    SMP_FAIL_SCHED_ADD_TASK             = 1ULL << 21,
    SMP_FAIL_SCHED_ENQUEUE              = 1ULL << 22,
    SMP_FAIL_SCHED_ATTEMPTS_EXHAUSTED   = 1ULL << 23,
    SMP_FAIL_SCHED_RUN_NEXT             = 1ULL << 24,
    SMP_FAIL_SCHED_TASK_NOT_ENTERED     = 1ULL << 25,
    SMP_FAIL_SCHED_PROCESS_MISMATCH     = 1ULL << 26,
};

enum smp_pidvfs_task_fail_stage
{
    SMP_PIDVFS_TASK_FAIL_NONE = 0,
    SMP_PIDVFS_TASK_FAIL_CURRENT_PROCESS,
    SMP_PIDVFS_TASK_FAIL_OPEN,
    SMP_PIDVFS_TASK_FAIL_ADD_FD,
    SMP_PIDVFS_TASK_FAIL_READ,
    SMP_PIDVFS_TASK_FAIL_PAYLOAD,
    SMP_PIDVFS_TASK_FAIL_SLOT_LAYOUT,
    SMP_PIDVFS_TASK_FAIL_SEEK_WRITE,
    SMP_PIDVFS_TASK_FAIL_WRITE,
    SMP_PIDVFS_TASK_FAIL_SEEK_VERIFY,
    SMP_PIDVFS_TASK_FAIL_VERIFY,
    SMP_PIDVFS_TASK_FAIL_CLOSE,
};

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
        ctx->failStage = SMP_PIDVFS_TASK_FAIL_CURRENT_PROCESS;
        localFail = 1;
    }

    if (!localFail)
    {
        opened = ctx->vfs->Open(start, "/test.txt", 0);
        if (opened == nullptr)
        {
            ctx->failStage = SMP_PIDVFS_TASK_FAIL_OPEN;
            localFail = 1;
        }
    }

    if (!localFail)
    {
        fd = ctx->processManager->AddFileDescriptor(currentProcess, (file_handle_t) opened, FD_FLAG_NONE);
        if (fd < 0)
        {
            ctx->failStage = SMP_PIDVFS_TASK_FAIL_ADD_FD;
            localFail = 1;
        }
    }

    int64_t fileBytes = -1;
    if (!localFail)
    {
        fileBytes = ctx->vfs->Read(opened, probe, sizeof(probe));
        if (fileBytes <= 0)
        {
            ctx->failStage = SMP_PIDVFS_TASK_FAIL_READ;
            localFail = 1;
        }
    }

    size_t payloadLen = 0;
    if (!localFail)
    {
        payloadLen = format_pid_payload(ctx->processId, payload, sizeof(payload));
        if (payloadLen == 0)
        {
            ctx->failStage = SMP_PIDVFS_TASK_FAIL_PAYLOAD;
            localFail = 1;
        }
    }

    uint64_t slotSize         = 16;
    const uint64_t cpuCount   = platform.cpu_count == 0 ? 1 : platform.cpu_count;
    const uint64_t defaultMin = slotSize * cpuCount;
    if ((uint64_t) fileBytes < defaultMin)
    {
        slotSize = (uint64_t) fileBytes / cpuCount;
    }

    const uint64_t slotOffset = ctx->cpuId * slotSize;
    if (!localFail && (slotSize == 0 || payloadLen > slotSize || slotOffset + (uint64_t) payloadLen > (uint64_t) fileBytes))
    {
        ctx->failStage = SMP_PIDVFS_TASK_FAIL_SLOT_LAYOUT;
        localFail = 1;
    }

    if (!localFail && ctx->vfs->Seek(opened, (int64_t) slotOffset, 0) < 0)
    {
        ctx->failStage = SMP_PIDVFS_TASK_FAIL_SEEK_WRITE;
        localFail = 1;
    }

    if (!localFail && ctx->vfs->Write(opened, payload, (uint64_t) payloadLen) != (int64_t) payloadLen)
    {
        ctx->failStage = SMP_PIDVFS_TASK_FAIL_WRITE;
        localFail = 1;
    }

    if (!localFail && ctx->vfs->Seek(opened, (int64_t) slotOffset, 0) < 0)
    {
        ctx->failStage = SMP_PIDVFS_TASK_FAIL_SEEK_VERIFY;
        localFail = 1;
    }

    if (!localFail)
    {
        const int64_t verifyBytes = ctx->vfs->Read(opened, verify, (uint64_t) payloadLen);
        if (verifyBytes != (int64_t) payloadLen || memcmp(verify, payload, payloadLen) != 0)
        {
            ctx->failStage = SMP_PIDVFS_TASK_FAIL_VERIFY;
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
            ctx->failStage = SMP_PIDVFS_TASK_FAIL_CLOSE;
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

extern "C" void smp_selftests(void)
{
    const uint64_t cpuId = (uint64_t) arch_cpu_id();
    smp_test_totals_t totals            = {};
    unsigned long long localFailMask    = 0;
    ResourceLayerCaps* resourceLayerCaps = nullptr;
    LogicLayerCaps*    logicLayerCaps    = nullptr;

    Dispatcher* dispatcher = (Dispatcher*) platform.dispacher;
    if (dispatcher == nullptr)
    {
        kprintf("Arx kernel: smp pid-vfs test skipped cpu=%u (dispatcher null)\n", (unsigned) cpuId);
        KDEBUG("smp pid-vfs test skipped cpu=%u (dispatcher null)\n", (unsigned) cpuId);
        totals.fails++;
        localFailMask |= SMP_FAIL_DISPATCHER_NULL;
    }
    else
    {
        resourceLayerCaps = dispatcher->GetResourceLayerCaps();
        logicLayerCaps    = dispatcher->GetLogicLayerCaps();
        if (resourceLayerCaps == nullptr || logicLayerCaps == nullptr || resourceLayerCaps->taskManager == nullptr || resourceLayerCaps->processManager == nullptr || logicLayerCaps->virtualFileSystem == nullptr)
        {
            kprintf("Arx kernel: smp pid-vfs test skipped cpu=%u (missing managers)\n", (unsigned) cpuId);
            KDEBUG("smp pid-vfs test skipped cpu=%u (missing managers)\n", (unsigned) cpuId);
            totals.fails++;
            localFailMask |= SMP_FAIL_MANAGERS_MISSING;
        }
        else
        {

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

            do
            {
                if (returnTask == nullptr)
                {
                    fails++;
                    localFailMask |= SMP_FAIL_PIDVFS_MISSING_RUNNING;
                    kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u missing running task\n", (unsigned) cpuId);
                    KDEBUG("smp pid-vfs FAIL cpu=%u missing running task\n", (unsigned) cpuId);
                    break;
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
                    localFailMask |= SMP_FAIL_PIDVFS_MISSING_ADDR_SPACE;
                    kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u missing address space\n", (unsigned) cpuId);
                    KDEBUG("smp pid-vfs FAIL cpu=%u missing address space\n", (unsigned) cpuId);
                    break;
                }

                process = processManager->CreateProcess(activeSpace);
                if (process == nullptr)
                {
                    fails++;
                    localFailMask |= SMP_FAIL_PIDVFS_CREATE_PROCESS;
                    kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u create process failed\n", (unsigned) cpuId);
                    KDEBUG("smp pid-vfs FAIL cpu=%u create process failed\n", (unsigned) cpuId);
                    break;
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
                    localFailMask |= SMP_FAIL_PIDVFS_CREATE_TASK;
                    kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u create task failed\n", (unsigned) cpuId);
                    KDEBUG("smp pid-vfs FAIL cpu=%u create task failed\n", (unsigned) cpuId);
                    break;
                }

                if (!processManager->AddTask(process, task))
                {
                    fails++;
                    localFailMask |= SMP_FAIL_PIDVFS_ADD_TASK;
                    kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u add task failed\n", (unsigned) cpuId);
                    KDEBUG("smp pid-vfs FAIL cpu=%u add task failed\n", (unsigned) cpuId);
                    break;
                }

                if (!processManager->SetRunningProcess((uint8_t) cpuId, process))
                {
                    fails++;
                    localFailMask |= SMP_FAIL_PIDVFS_SET_RUNNING;
                    kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u set running process failed\n", (unsigned) cpuId);
                    KDEBUG("smp pid-vfs FAIL cpu=%u set running process failed\n", (unsigned) cpuId);
                    break;
                }

                if (!taskManager->ExecuteTask(task))
                {
                    fails++;
                    localFailMask |= SMP_FAIL_PIDVFS_EXECUTE;
                    kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u execute task failed\n", (unsigned) cpuId);
                    KDEBUG("smp pid-vfs FAIL cpu=%u execute task failed\n", (unsigned) cpuId);
                    break;
                }

                if (!processManager->SetRunningProcess((uint8_t) cpuId, originalRun))
                {
                    fails++;
                    localFailMask |= SMP_FAIL_PIDVFS_RESTORE_RUNNING;
                    kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u restore running process failed\n", (unsigned) cpuId);
                    KDEBUG("smp pid-vfs FAIL cpu=%u restore running process failed\n", (unsigned) cpuId);
                    break;
                }

                if (ctx.completed != 1 || ctx.passed != 1)
                {
                    fails++;
                    localFailMask |= SMP_FAIL_PIDVFS_TASK_BODY;
                    if (ctx.failStage == SMP_PIDVFS_TASK_FAIL_SLOT_LAYOUT)
                    {
                        localFailMask |= SMP_FAIL_PIDVFS_SLOT_LAYOUT;
                    }
                    kprintf("Arx kernel: smp pid-vfs FAIL cpu=%u completed=%d passed=%d\n", (unsigned) cpuId, ctx.completed, ctx.passed);
                    KDEBUG("smp pid-vfs FAIL cpu=%u completed=%d passed=%d\n", (unsigned) cpuId, ctx.completed, ctx.passed);
                    KDEBUG("smp pid-vfs FAIL cpu=%u task_fail_stage=%d\n", (unsigned) cpuId, (int) ctx.failStage);
                    break;
                }

                passes++;
                kprintf("Arx kernel: smp pid-vfs PASS cpu=%u pid=%llu\n", (unsigned) cpuId, (unsigned long long) ctx.processId);
                KDEBUG("smp pid-vfs PASS cpu=%u pid=%llu\n", (unsigned) cpuId, (unsigned long long) ctx.processId);
            } while (0);

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

            totals.passes += passes;
            totals.fails += fails;

            unsigned long long schedulerPasses = 0;
            unsigned long long schedulerFails  = 0;
            unsigned long long schedulerFailMask = 0;
            run_smp_scheduler_selftest((void*) logicLayerCaps, (void*) resourceLayerCaps, (unsigned long long) cpuId, &schedulerPasses, &schedulerFails, &schedulerFailMask);
            if (schedulerFailMask & (1ULL << 0))
                localFailMask |= SMP_FAIL_SCHED_MISSING_MANAGERS;
            if (schedulerFailMask & (1ULL << 1))
                localFailMask |= SMP_FAIL_SCHED_MISSING_RUNNING;
            if (schedulerFailMask & (1ULL << 2))
                localFailMask |= SMP_FAIL_SCHED_MISSING_ADDR_SPACE;
            if (schedulerFailMask & (1ULL << 3))
                localFailMask |= SMP_FAIL_SCHED_CREATE_PROCESS;
            if (schedulerFailMask & (1ULL << 4))
                localFailMask |= SMP_FAIL_SCHED_CREATE_TASK;
            if (schedulerFailMask & (1ULL << 5))
                localFailMask |= SMP_FAIL_SCHED_ADD_TASK;
            if (schedulerFailMask & (1ULL << 6))
                localFailMask |= SMP_FAIL_SCHED_ENQUEUE;
            if (schedulerFailMask & (1ULL << 7))
                localFailMask |= SMP_FAIL_SCHED_ATTEMPTS_EXHAUSTED;
            if (schedulerFailMask & (1ULL << 8))
                localFailMask |= SMP_FAIL_SCHED_RUN_NEXT;
            if (schedulerFailMask & (1ULL << 9))
                localFailMask |= SMP_FAIL_SCHED_TASK_NOT_ENTERED;
            if (schedulerFailMask & (1ULL << 10))
                localFailMask |= SMP_FAIL_SCHED_PROCESS_MISMATCH;
            totals.passes += schedulerPasses;
            totals.fails += schedulerFails;
        }
    }
    (void) __atomic_fetch_add(&g_smp_selftests_total_passes, totals.passes, __ATOMIC_RELAXED);
    (void) __atomic_fetch_add(&g_smp_selftests_total_fails, totals.fails, __ATOMIC_RELAXED);
    (void) __atomic_fetch_or(&g_smp_selftests_fail_mask, localFailMask, __ATOMIC_RELAXED);
    (void) __atomic_add_fetch(&g_smp_selftests_finished_cpus, 1ULL, __ATOMIC_ACQ_REL);
}

extern "C" void smp_selftests_wait_for_all_cpus(void)
{
    const unsigned long long expected = (unsigned long long) platform.cpu_count;
    while (__atomic_load_n(&g_smp_selftests_finished_cpus, __ATOMIC_ACQUIRE) < expected)
    {
        arch_pause();
    }
}

extern "C" void smp_selftests_get_totals(unsigned long long* out_passes, unsigned long long* out_fails, unsigned long long* out_finished_cpus)
{
    if (out_passes != nullptr)
    {
        *out_passes = __atomic_load_n(&g_smp_selftests_total_passes, __ATOMIC_RELAXED);
    }

    if (out_fails != nullptr)
    {
        *out_fails = __atomic_load_n(&g_smp_selftests_total_fails, __ATOMIC_RELAXED);
    }

    if (out_finished_cpus != nullptr)
    {
        *out_finished_cpus = __atomic_load_n(&g_smp_selftests_finished_cpus, __ATOMIC_ACQUIRE);
    }
}

extern "C" void smp_selftests_kdebug_summary_details(void)
{
    const unsigned long long mask = __atomic_load_n(&g_smp_selftests_fail_mask, __ATOMIC_RELAXED);
    if (mask == 0)
    {
        KDEBUG("smp summary detail: no failure mask bits set\n");
        return;
    }

    KDEBUG("smp summary detail: fail_mask=0x%llx\n", mask);

    if (mask & SMP_FAIL_DISPATCHER_NULL)
        KDEBUG("smp summary detail: dispatcher null\n");
    if (mask & SMP_FAIL_MANAGERS_MISSING)
        KDEBUG("smp summary detail: managers missing\n");
    if (mask & SMP_FAIL_PIDVFS_MISSING_RUNNING)
        KDEBUG("smp summary detail: pid-vfs missing running task\n");
    if (mask & SMP_FAIL_PIDVFS_MISSING_ADDR_SPACE)
        KDEBUG("smp summary detail: pid-vfs missing address space\n");
    if (mask & SMP_FAIL_PIDVFS_CREATE_PROCESS)
        KDEBUG("smp summary detail: pid-vfs create process failed\n");
    if (mask & SMP_FAIL_PIDVFS_CREATE_TASK)
        KDEBUG("smp summary detail: pid-vfs create task failed\n");
    if (mask & SMP_FAIL_PIDVFS_ADD_TASK)
        KDEBUG("smp summary detail: pid-vfs add task failed\n");
    if (mask & SMP_FAIL_PIDVFS_SET_RUNNING)
        KDEBUG("smp summary detail: pid-vfs set running process failed\n");
    if (mask & SMP_FAIL_PIDVFS_EXECUTE)
        KDEBUG("smp summary detail: pid-vfs execute task failed\n");
    if (mask & SMP_FAIL_PIDVFS_RESTORE_RUNNING)
        KDEBUG("smp summary detail: pid-vfs restore running process failed\n");
    if (mask & SMP_FAIL_PIDVFS_TASK_BODY)
        KDEBUG("smp summary detail: pid-vfs task body reported failure\n");
    if (mask & SMP_FAIL_PIDVFS_SLOT_LAYOUT)
        KDEBUG("smp summary detail: pid-vfs slot layout/file size insufficient for cpu count\n");

    if (mask & SMP_FAIL_SCHED_MISSING_MANAGERS)
        KDEBUG("smp summary detail: scheduler managers missing\n");
    if (mask & SMP_FAIL_SCHED_MISSING_RUNNING)
        KDEBUG("smp summary detail: scheduler missing running task\n");
    if (mask & SMP_FAIL_SCHED_MISSING_ADDR_SPACE)
        KDEBUG("smp summary detail: scheduler missing address space\n");
    if (mask & SMP_FAIL_SCHED_CREATE_PROCESS)
        KDEBUG("smp summary detail: scheduler create process failed\n");
    if (mask & SMP_FAIL_SCHED_CREATE_TASK)
        KDEBUG("smp summary detail: scheduler create task failed\n");
    if (mask & SMP_FAIL_SCHED_ADD_TASK)
        KDEBUG("smp summary detail: scheduler add task failed\n");
    if (mask & SMP_FAIL_SCHED_ENQUEUE)
        KDEBUG("smp summary detail: scheduler enqueue failed\n");
    if (mask & SMP_FAIL_SCHED_ATTEMPTS_EXHAUSTED)
        KDEBUG("smp summary detail: scheduler dispatch attempts exhausted\n");
    if (mask & SMP_FAIL_SCHED_RUN_NEXT)
        KDEBUG("smp summary detail: scheduler RunNextReadyProcess failed\n");
    if (mask & SMP_FAIL_SCHED_TASK_NOT_ENTERED)
        KDEBUG("smp summary detail: scheduler test task not entered\n");
    if (mask & SMP_FAIL_SCHED_PROCESS_MISMATCH)
        KDEBUG("smp summary detail: scheduler process mismatch\n");
}

extern "C" void smp_selftests_record_failure_details(void)
{
    const unsigned long long mask = __atomic_load_n(&g_smp_selftests_fail_mask, __ATOMIC_RELAXED);
    if (mask == 0)
    {
        return;
    }

    if (mask & SMP_FAIL_DISPATCHER_NULL)
        selftest_record_failure_detail("dispatcher null");
    if (mask & SMP_FAIL_MANAGERS_MISSING)
        selftest_record_failure_detail("managers missing");
    if (mask & SMP_FAIL_PIDVFS_MISSING_RUNNING)
        selftest_record_failure_detail("pid-vfs missing running task");
    if (mask & SMP_FAIL_PIDVFS_MISSING_ADDR_SPACE)
        selftest_record_failure_detail("pid-vfs missing address space");
    if (mask & SMP_FAIL_PIDVFS_CREATE_PROCESS)
        selftest_record_failure_detail("pid-vfs create process failed");
    if (mask & SMP_FAIL_PIDVFS_CREATE_TASK)
        selftest_record_failure_detail("pid-vfs create task failed");
    if (mask & SMP_FAIL_PIDVFS_ADD_TASK)
        selftest_record_failure_detail("pid-vfs add task failed");
    if (mask & SMP_FAIL_PIDVFS_SET_RUNNING)
        selftest_record_failure_detail("pid-vfs set running process failed");
    if (mask & SMP_FAIL_PIDVFS_EXECUTE)
        selftest_record_failure_detail("pid-vfs execute task failed");
    if (mask & SMP_FAIL_PIDVFS_RESTORE_RUNNING)
        selftest_record_failure_detail("pid-vfs restore running process failed");
    if (mask & SMP_FAIL_PIDVFS_TASK_BODY)
        selftest_record_failure_detail("pid-vfs task body reported failure");
    if (mask & SMP_FAIL_PIDVFS_SLOT_LAYOUT)
        selftest_record_failure_detail("pid-vfs slot layout/file size insufficient for cpu count");

    if (mask & SMP_FAIL_SCHED_MISSING_MANAGERS)
        selftest_record_failure_detail("scheduler managers missing");
    if (mask & SMP_FAIL_SCHED_MISSING_RUNNING)
        selftest_record_failure_detail("scheduler missing running task");
    if (mask & SMP_FAIL_SCHED_MISSING_ADDR_SPACE)
        selftest_record_failure_detail("scheduler missing address space");
    if (mask & SMP_FAIL_SCHED_CREATE_PROCESS)
        selftest_record_failure_detail("scheduler create process failed");
    if (mask & SMP_FAIL_SCHED_CREATE_TASK)
        selftest_record_failure_detail("scheduler create task failed");
    if (mask & SMP_FAIL_SCHED_ADD_TASK)
        selftest_record_failure_detail("scheduler add task failed");
    if (mask & SMP_FAIL_SCHED_ENQUEUE)
        selftest_record_failure_detail("scheduler enqueue failed");
    if (mask & SMP_FAIL_SCHED_ATTEMPTS_EXHAUSTED)
        selftest_record_failure_detail("scheduler dispatch attempts exhausted");
    if (mask & SMP_FAIL_SCHED_RUN_NEXT)
        selftest_record_failure_detail("scheduler RunNextReadyProcess failed");
    if (mask & SMP_FAIL_SCHED_TASK_NOT_ENTERED)
        selftest_record_failure_detail("scheduler test task not entered");
    if (mask & SMP_FAIL_SCHED_PROCESS_MISMATCH)
        selftest_record_failure_detail("scheduler process mismatch");
}
