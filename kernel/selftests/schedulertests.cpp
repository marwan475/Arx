#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <arch/arch.h>
#include <boot/boot.h>
#include <klib/klib.h>
#include <platform.h>
#include <selftests/selftests.h>
}

namespace
{
enum scheduler_fail_mask_bits
{
    SCHED_FAIL_MISSING_MANAGERS            = 1ULL << 0,
    SCHED_FAIL_MISSING_RUNNING_TASK        = 1ULL << 1,
    SCHED_FAIL_MISSING_ADDRESS_SPACE       = 1ULL << 2,
    SCHED_FAIL_CREATE_PROCESS              = 1ULL << 3,
    SCHED_FAIL_CREATE_TASK                 = 1ULL << 4,
    SCHED_FAIL_ADD_TASK                    = 1ULL << 5,
    SCHED_FAIL_ENQUEUE                     = 1ULL << 6,
    SCHED_FAIL_DISPATCH_ATTEMPTS_EXHAUSTED = 1ULL << 7,
    SCHED_FAIL_RUN_NEXT                    = 1ULL << 8,
    SCHED_FAIL_TASK_NOT_ENTERED            = 1ULL << 9,
    SCHED_FAIL_PROCESS_MISMATCH            = 1ULL << 10,
};

struct smp_scheduler_task_context_t
{
    ProcessManager* processManager;
    TaskManager*    taskManager;
    task_t*         returnTask;
    uint64_t        cpuId;
    uint64_t        processIndex;
    uint64_t        expectedProcessId;
    volatile int    entered;
    volatile int    processMatched;
};

static void smp_scheduler_task(void* arg)
{
    smp_scheduler_task_context_t* ctx = (smp_scheduler_task_context_t*) arg;
    if (ctx == nullptr || ctx->processManager == nullptr || ctx->taskManager == nullptr || ctx->returnTask == nullptr)
    {
        for (;;)
        {
            arch_pause();
        }
    }

    process_t* current = ctx->processManager->GetCurrentProcess();
    ctx->entered       = 1;
    if (current != nullptr && current->id == ctx->expectedProcessId)
    {
        ctx->processMatched = 1;
    }

    const unsigned long long actualProcessId = (current != nullptr) ? (unsigned long long) current->id : 0ULL;
    kprintf("Arx kernel: smp scheduler confirm cpu=%llu proc[%llu] expected_pid=%llu actual_pid=%llu match=%s\n", (unsigned long long) ctx->cpuId, (unsigned long long) ctx->processIndex,
            (unsigned long long) ctx->expectedProcessId, actualProcessId, ctx->processMatched ? "yes" : "no");
    KDEBUG("smp scheduler confirm cpu=%llu proc[%llu] expected_pid=%llu actual_pid=%llu match=%s\n", (unsigned long long) ctx->cpuId, (unsigned long long) ctx->processIndex,
           (unsigned long long) ctx->expectedProcessId, actualProcessId, ctx->processMatched ? "yes" : "no");

    (void) ctx->taskManager->ExecuteTask(ctx->returnTask);
    for (;;)
    {
        arch_pause();
    }
}
} // namespace

extern "C" void run_smp_scheduler_selftest(void* logicLayerCapsPtr, void* resourceLayerCapsPtr, unsigned long long cpuIdValue, unsigned long long* outPasses, unsigned long long* outFails,
                                             unsigned long long* outFailMask)
{
    if (outPasses != nullptr)
    {
        *outPasses = 0;
    }

    if (outFails != nullptr)
    {
        *outFails = 0;
    }

    if (outFailMask != nullptr)
    {
        *outFailMask = 0;
    }

    LogicLayerCaps*    logicLayerCaps    = (LogicLayerCaps*) logicLayerCapsPtr;
    ResourceLayerCaps* resourceLayerCaps = (ResourceLayerCaps*) resourceLayerCapsPtr;
    const uint64_t     cpuId             = (uint64_t) cpuIdValue;

    if (logicLayerCaps == nullptr || logicLayerCaps->scheduler == nullptr || resourceLayerCaps == nullptr || resourceLayerCaps->processManager == nullptr || resourceLayerCaps->taskManager == nullptr)
    {
        kprintf("Arx kernel: smp scheduler test skipped cpu=%u (missing managers)\n", (unsigned) cpuId);
        KDEBUG("smp scheduler test skipped cpu=%u (missing managers)\n", (unsigned) cpuId);
        if (outFails != nullptr)
        {
            (*outFails)++;
        }
        if (outFailMask != nullptr)
        {
            *outFailMask |= SCHED_FAIL_MISSING_MANAGERS;
        }
        return;
    }

    Scheduler*         scheduler      = logicLayerCaps->scheduler;
    ProcessManager*    processManager = resourceLayerCaps->processManager;
    TaskManager*       taskManager    = resourceLayerCaps->taskManager;
    process_t*         originalRun    = processManager->GetRunningProcess((uint8_t) cpuId);
    task_t*            returnTask     = taskManager->GetRunningTask((uint8_t) cpuId);
    virt_addr_space_t* activeSpace    = platform.cpus[cpuId].address_space;

    if (returnTask == nullptr)
    {
        kprintf("Arx kernel: smp scheduler FAIL cpu=%u missing running task\n", (unsigned) cpuId);
        KDEBUG("smp scheduler FAIL cpu=%u missing running task\n", (unsigned) cpuId);
        if (outFails != nullptr)
        {
            (*outFails)++;
        }
        if (outFailMask != nullptr)
        {
            *outFailMask |= SCHED_FAIL_MISSING_RUNNING_TASK;
        }
        return;
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
        kprintf("Arx kernel: smp scheduler FAIL cpu=%u missing address space\n", (unsigned) cpuId);
        KDEBUG("smp scheduler FAIL cpu=%u missing address space\n", (unsigned) cpuId);
        if (outFails != nullptr)
        {
            (*outFails)++;
        }
        if (outFailMask != nullptr)
        {
            *outFailMask |= SCHED_FAIL_MISSING_ADDRESS_SPACE;
        }
        return;
    }

    static constexpr size_t SCHED_TEST_PROCESS_COUNT = 3;
    process_t*              processes[SCHED_TEST_PROCESS_COUNT];
    task_t*                 tasks[SCHED_TEST_PROCESS_COUNT];
    smp_scheduler_task_context_t contexts[SCHED_TEST_PROCESS_COUNT];

    memset(processes, 0, sizeof(processes));
    memset(tasks, 0, sizeof(tasks));
    memset(contexts, 0, sizeof(contexts));

    unsigned long long passes = 0;
    unsigned long long fails  = 0;
    bool               abort  = false;

    kprintf("Arx kernel: smp scheduler start cpu=%u\n", (unsigned) cpuId);
    KDEBUG("smp scheduler start cpu=%u\n", (unsigned) cpuId);

    for (size_t i = 0; i < SCHED_TEST_PROCESS_COUNT; i++)
    {
        processes[i] = processManager->CreateProcess(activeSpace);
        if (processes[i] == nullptr)
        {
            fails++;
            if (outFailMask != nullptr)
            {
                *outFailMask |= SCHED_FAIL_CREATE_PROCESS;
            }
            kprintf("Arx kernel: smp scheduler FAIL cpu=%u create process[%llu] failed\n", (unsigned) cpuId, (unsigned long long) i);
            KDEBUG("smp scheduler FAIL cpu=%u create process[%llu] failed\n", (unsigned) cpuId, (unsigned long long) i);
            abort = true;
            break;
        }

        contexts[i].processManager    = processManager;
        contexts[i].taskManager       = taskManager;
        contexts[i].returnTask        = returnTask;
        contexts[i].cpuId             = cpuId;
        contexts[i].processIndex      = (uint64_t) i;
        contexts[i].expectedProcessId = processes[i]->id;

        tasks[i] = taskManager->CreateKernelTask(smp_scheduler_task, &contexts[i]);
        if (tasks[i] == nullptr)
        {
            fails++;
            if (outFailMask != nullptr)
            {
                *outFailMask |= SCHED_FAIL_CREATE_TASK;
            }
            kprintf("Arx kernel: smp scheduler FAIL cpu=%u create task[%llu] failed\n", (unsigned) cpuId, (unsigned long long) i);
            KDEBUG("smp scheduler FAIL cpu=%u create task[%llu] failed\n", (unsigned) cpuId, (unsigned long long) i);
            abort = true;
            break;
        }

        if (!processManager->AddTask(processes[i], tasks[i]))
        {
            fails++;
            if (outFailMask != nullptr)
            {
                *outFailMask |= SCHED_FAIL_ADD_TASK;
            }
            kprintf("Arx kernel: smp scheduler FAIL cpu=%u add task[%llu] failed\n", (unsigned) cpuId, (unsigned long long) i);
            KDEBUG("smp scheduler FAIL cpu=%u add task[%llu] failed\n", (unsigned) cpuId, (unsigned long long) i);
            abort = true;
            break;
        }

        if (!scheduler->EnqueueProcess((uint8_t) cpuId, processes[i]->id))
        {
            fails++;
            if (outFailMask != nullptr)
            {
                *outFailMask |= SCHED_FAIL_ENQUEUE;
            }
            kprintf("Arx kernel: smp scheduler FAIL cpu=%u enqueue process[%llu] pid=%llu failed\n", (unsigned) cpuId, (unsigned long long) i, (unsigned long long) processes[i]->id);
            KDEBUG("smp scheduler FAIL cpu=%u enqueue process[%llu] pid=%llu failed\n", (unsigned) cpuId, (unsigned long long) i, (unsigned long long) processes[i]->id);
            abort = true;
            break;
        }
    }

    size_t dispatchAttempts            = 0;
    const size_t maxDispatchAttempts   = SCHED_TEST_PROCESS_COUNT + 8;
    while (!abort)
    {
        size_t enteredCount = 0;
        for (size_t i = 0; i < SCHED_TEST_PROCESS_COUNT; i++)
        {
            if (contexts[i].entered == 1)
            {
                enteredCount++;
            }
        }

        if (enteredCount >= SCHED_TEST_PROCESS_COUNT)
        {
            break;
        }

        if (dispatchAttempts >= maxDispatchAttempts)
        {
            fails++;
            if (outFailMask != nullptr)
            {
                *outFailMask |= SCHED_FAIL_DISPATCH_ATTEMPTS_EXHAUSTED;
            }
            kprintf("Arx kernel: smp scheduler FAIL cpu=%u dispatch attempts exhausted entered=%llu expected=%llu\n", (unsigned) cpuId, (unsigned long long) enteredCount, (unsigned long long) SCHED_TEST_PROCESS_COUNT);
            KDEBUG("smp scheduler FAIL cpu=%u dispatch attempts exhausted entered=%llu expected=%llu\n", (unsigned) cpuId, (unsigned long long) enteredCount, (unsigned long long) SCHED_TEST_PROCESS_COUNT);
            abort = true;
            break;
        }

        if (!scheduler->RunNextReadyProcess((uint8_t) cpuId))
        {
            fails++;
            if (outFailMask != nullptr)
            {
                *outFailMask |= SCHED_FAIL_RUN_NEXT;
            }
            kprintf("Arx kernel: smp scheduler FAIL cpu=%u run ready process attempt=%llu failed\n", (unsigned) cpuId, (unsigned long long) dispatchAttempts);
            KDEBUG("smp scheduler FAIL cpu=%u run ready process attempt=%llu failed\n", (unsigned) cpuId, (unsigned long long) dispatchAttempts);
            abort = true;
            break;
        }

        dispatchAttempts++;
    }

    for (size_t i = 0; i < SCHED_TEST_PROCESS_COUNT && !abort; i++)
    {
        if (contexts[i].entered != 1)
        {
            fails++;
            if (outFailMask != nullptr)
            {
                *outFailMask |= SCHED_FAIL_TASK_NOT_ENTERED;
            }
            kprintf("Arx kernel: smp scheduler FAIL cpu=%u task[%llu] never entered\n", (unsigned) cpuId, (unsigned long long) i);
            KDEBUG("smp scheduler FAIL cpu=%u task[%llu] never entered\n", (unsigned) cpuId, (unsigned long long) i);
            continue;
        }

        if (contexts[i].processMatched != 1)
        {
            fails++;
            if (outFailMask != nullptr)
            {
                *outFailMask |= SCHED_FAIL_PROCESS_MISMATCH;
            }
            kprintf("Arx kernel: smp scheduler FAIL cpu=%u task[%llu] process mismatch expected=%llu\n", (unsigned) cpuId, (unsigned long long) i, (unsigned long long) contexts[i].expectedProcessId);
            KDEBUG("smp scheduler FAIL cpu=%u task[%llu] process mismatch expected=%llu\n", (unsigned) cpuId, (unsigned long long) i, (unsigned long long) contexts[i].expectedProcessId);
            continue;
        }

        passes++;
    }

    if (processManager->GetRunningProcess((uint8_t) cpuId) != originalRun)
    {
        (void) processManager->SetRunningProcess((uint8_t) cpuId, originalRun);
    }

    for (size_t i = 0; i < SCHED_TEST_PROCESS_COUNT; i++)
    {
        if (tasks[i] != nullptr)
        {
            (void) taskManager->FreeTask(tasks[i]);
        }

        if (processes[i] != nullptr)
        {
            (void) processManager->FreeProcess(processes[i]);
        }
    }

    if (outPasses != nullptr)
    {
        *outPasses += passes;
    }

    if (outFails != nullptr)
    {
        *outFails += fails;
    }
}
