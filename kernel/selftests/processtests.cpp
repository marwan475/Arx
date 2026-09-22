#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
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

struct process_test_context_t
{
    TaskManager*        manager;
    ProcessManager*     processManager;
    task_t*             bspTask;
    process_t*          processA;
    process_t*          processB;
    virt_addr_space_t*  expectedSpaceA;
    virt_addr_space_t*  expectedSpaceB;
    volatile int        markerA;
    volatile int        markerB;
    volatile int        processCheckA;
    volatile int        processCheckB;
    volatile int        addressSpaceCheckA;
    volatile int        addressSpaceCheckB;
};

static process_test_context_t g_process_test_ctx;

static void process_task_a(void* arg)
{
    process_test_context_t* ctx = static_cast<process_test_context_t*>(arg);
    process_t* currentProcess = ctx->processManager->GetCurrentProcess();
    ctx->processCheckA = (currentProcess == ctx->processA) ? 1 : 0;
    ctx->addressSpaceCheckA = (platform.cpus[arch_cpu_id()].address_space == ctx->expectedSpaceA) ? 1 : 0;

    kprintf("Arx kernel: process_selftest task A entered\n");
    kprintf("Arx kernel: process_selftest task A checks: process=%s address_space=%s\n", ctx->processCheckA == 1 ? "OK" : "BAD", ctx->addressSpaceCheckA == 1 ? "OK" : "BAD");
    ctx->markerA                = 1;
    kprintf("Arx kernel: process_selftest task A switching back to BSP\n");
    ctx->manager->ExecuteTask(ctx->bspTask);

    for (;;)
    {
        arch_pause();
    }
}

static void process_task_b(void* arg)
{
    process_test_context_t* ctx = static_cast<process_test_context_t*>(arg);
    process_t* currentProcess = ctx->processManager->GetCurrentProcess();
    ctx->processCheckB = (currentProcess == ctx->processB) ? 1 : 0;
    ctx->addressSpaceCheckB = (platform.cpus[arch_cpu_id()].address_space == ctx->expectedSpaceB) ? 1 : 0;

    kprintf("Arx kernel: process_selftest task B entered\n");
    kprintf("Arx kernel: process_selftest task B checks: process=%s address_space=%s\n", ctx->processCheckB == 1 ? "OK" : "BAD", ctx->addressSpaceCheckB == 1 ? "OK" : "BAD");
    ctx->markerB                = 1;
    kprintf("Arx kernel: process_selftest task B switching back to BSP\n");
    ctx->manager->ExecuteTask(ctx->bspTask);

    for (;;)
    {
        arch_pause();
    }
}

extern "C" void run_process_selftests(void* resourceLayerCaps, void* logicLayerCaps)
{
    unsigned long long passes = 0;
    unsigned long long fails  = 0;
    Scheduler* scheduler = nullptr;
    ProcessManager* processManager = nullptr;
    TaskManager*    taskManager    = nullptr;
    process_t* processA  = nullptr;
    process_t* processB  = nullptr;
    task_t*    taskA     = nullptr;
    task_t*    taskB     = nullptr;
    virt_addr_space_t* currentSpace = nullptr;

    kprintf("Arx kernel: process_selftest start\n");

    ResourceLayerCaps* caps = static_cast<ResourceLayerCaps*>(resourceLayerCaps);
    LogicLayerCaps*    logicCaps = static_cast<LogicLayerCaps*>(logicLayerCaps);
    if (caps == nullptr || caps->processManager == nullptr || caps->taskManager == nullptr)
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: missing managers\n");
        goto done;
    }

    if (logicCaps == nullptr || logicCaps->scheduler == nullptr)
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: missing logic scheduler\n");
        goto done;
    }

    processManager = caps->processManager;
    taskManager    = caps->taskManager;

    g_process_test_ctx.manager = taskManager;
    g_process_test_ctx.processManager = processManager;
    g_process_test_ctx.bspTask = taskManager->GetCurrentTask();
    g_process_test_ctx.processA = nullptr;
    g_process_test_ctx.processB = nullptr;
    g_process_test_ctx.expectedSpaceA = nullptr;
    g_process_test_ctx.expectedSpaceB = nullptr;
    g_process_test_ctx.markerA = 0;
    g_process_test_ctx.markerB = 0;
    g_process_test_ctx.processCheckA = 0;
    g_process_test_ctx.processCheckB = 0;
    g_process_test_ctx.addressSpaceCheckA = 0;
    g_process_test_ctx.addressSpaceCheckB = 0;

    if (g_process_test_ctx.bspTask == nullptr)
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: missing BSP task\n");
        goto done;
    }

    scheduler = logicCaps->scheduler;

    currentSpace = platform.cpus[arch_cpu_id()].address_space;
    if (currentSpace == nullptr)
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: missing current address space\n");
        goto done;
    }

    processA = processManager->CreateProcess(currentSpace);
    processB = processManager->CreateProcess(currentSpace);

    if (processA == nullptr || processB == nullptr)
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: failed to create processes\n");
        goto cleanup;
    }

    g_process_test_ctx.processA = processA;
    g_process_test_ctx.processB = processB;
    g_process_test_ctx.expectedSpaceA = processA->addressSpace;
    g_process_test_ctx.expectedSpaceB = processB->addressSpace;

    taskA = taskManager->CreateKernelTask(process_task_a, &g_process_test_ctx);
    taskB = taskManager->CreateKernelTask(process_task_b, &g_process_test_ctx);
    if (taskA == nullptr || taskB == nullptr)
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: failed to create tasks\n");
        goto cleanup;
    }

    if (!processManager->AddTask(processA, taskA) || !processManager->AddTask(processB, taskB))
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: failed to attach tasks to processes\n");
        goto cleanup;
    }

    if (!scheduler->ScheduleProcess(processA->id))
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: ScheduleProcess(processA) failed\n");
        goto cleanup;
    }

    if (!scheduler->ScheduleProcess(processB->id))
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: ScheduleProcess(processB) failed\n");
        goto cleanup;
    }

    if (g_process_test_ctx.markerA != 1 || g_process_test_ctx.markerB != 1)
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: expected markers A=1 B=1, got A=%d B=%d\n", g_process_test_ctx.markerA, g_process_test_ctx.markerB);
        goto cleanup;
    }

    if (g_process_test_ctx.processCheckA != 1 || g_process_test_ctx.processCheckB != 1)
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: process checks A=%d B=%d\n", g_process_test_ctx.processCheckA, g_process_test_ctx.processCheckB);
        goto cleanup;
    }

    if (g_process_test_ctx.addressSpaceCheckA != 1 || g_process_test_ctx.addressSpaceCheckB != 1)
    {
        fails++;
        kprintf("Arx kernel: process_selftest FAIL: address-space checks A=%d B=%d\n", g_process_test_ctx.addressSpaceCheckA, g_process_test_ctx.addressSpaceCheckB);
        goto cleanup;
    }

    passes++;
    kprintf("Arx kernel: process_selftest PASS: scheduled two processes\n");

cleanup:
    if (taskA != nullptr)
    {
        taskManager->FreeTask(taskA);
    }

    if (taskB != nullptr)
    {
        taskManager->FreeTask(taskB);
    }

    if (processA != nullptr)
    {
        processManager->FreeProcess(processA);
    }

    if (processB != nullptr)
    {
        processManager->FreeProcess(processB);
    }

done:
    kprintf("Arx kernel: process_selftest summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) fails);
    kprintf("Arx kernel: process_selftest RESULT=%s\n", fails == 0 ? "PASS" : "FAIL");
    if (fails == 0)
    {
        KDEBUG("process_selftest passed with %llu checks\n", (unsigned long long) passes);
    }
    else
    {
        KDEBUG("process_selftest failed with %llu checks\n", (unsigned long long) fails);
    }
}
