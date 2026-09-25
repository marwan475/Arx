// Basic task context-switch selftest between two task entry functions.

#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <klib/klib.h>
#include <selftests/selftests.h>
}

struct task_test_context_t
{
    TaskManager* manager;
    task_t*      taskA;
    task_t*      taskB;
    task_t*      bspTask;
    volatile int step;
};

static task_test_context_t g_task_test_ctx;

static void task_entry_a(void* arg)
{
    task_test_context_t* ctx = static_cast<task_test_context_t*>(arg);

    kprintf("Arx kernel: task_selftest task A entered\n");
    ctx->step = 1;
    kprintf("Arx kernel: task_selftest switching A -> B\n");
    ctx->manager->ExecuteTask(ctx->taskB);

    // If task B switched back to task A, we reach here.
    kprintf("Arx kernel: task_selftest task A resumed\n");
    ctx->step = 3;
    kprintf("Arx kernel: task_selftest switching A -> BSP\n");
    ctx->manager->ExecuteTask(ctx->bspTask);

    for (;;)
    {
        arch_pause();
    }
}

static void task_entry_b(void* arg)
{
    task_test_context_t* ctx = static_cast<task_test_context_t*>(arg);

    kprintf("Arx kernel: task_selftest task B entered\n");
    ctx->step = 2;
    kprintf("Arx kernel: task_selftest switching B -> A\n");
    ctx->manager->ExecuteTask(ctx->taskA);

    for (;;)
    {
        arch_pause();
    }
}

extern "C" void run_task_selftests(void* resourceLayerCaps)
{
    unsigned long long passes = 0;
    unsigned long long fails  = 0;
    TaskManager* manager = nullptr;
    task_t*      taskA   = nullptr;
    task_t*      taskB   = nullptr;
    task_t*      bspTask = nullptr;
    task_t*      originalRunningTask = nullptr;
    task_t*      surrogateTask       = nullptr;
    auto         cpuId               = arch_cpu_id();

    selftest_case_begin("task_selftest");

    ResourceLayerCaps* caps = static_cast<ResourceLayerCaps*>(resourceLayerCaps);
    if (caps == nullptr || caps->taskManager == nullptr)
    {
        fails++;
        kprintf("Arx kernel: task_selftest FAIL: missing resource layer task manager\n");
        goto done;
    }

    manager = caps->taskManager;

    originalRunningTask = manager->GetRunningTask(cpuId);
    bspTask             = originalRunningTask;
    if (bspTask == nullptr)
    {
        surrogateTask = manager->AllocateTask();
        if (surrogateTask == nullptr)
        {
            fails++;
            kprintf("Arx kernel: task_selftest FAIL: failed to allocate surrogate running task\n");
            goto done;
        }

        if (!manager->SetRunningTask(cpuId, surrogateTask))
        {
            fails++;
            kprintf("Arx kernel: task_selftest FAIL: failed to set surrogate running task\n");
            goto cleanup;
        }

        bspTask = surrogateTask;
        kprintf("Arx kernel: task_selftest INFO: using surrogate running task\n");
    }

    g_task_test_ctx.manager = manager;
    g_task_test_ctx.taskA   = nullptr;
    g_task_test_ctx.taskB   = nullptr;
    g_task_test_ctx.bspTask = bspTask;
    g_task_test_ctx.step    = 0;

    taskA = manager->CreateKernelTask(task_entry_a, &g_task_test_ctx);
    taskB = manager->CreateKernelTask(task_entry_b, &g_task_test_ctx);
    g_task_test_ctx.taskA = taskA;
    g_task_test_ctx.taskB = taskB;

    if (taskA == nullptr || taskB == nullptr)
    {
        fails++;
        kprintf("Arx kernel: task_selftest FAIL: failed to create task pair\n");
        goto cleanup;
    }

    kprintf("Arx kernel: task_selftest created tasks A(id=%llu) B(id=%llu)\n", (unsigned long long) taskA->id, (unsigned long long) taskB->id);

    if (!manager->ExecuteTask(taskA))
    {
        fails++;
        kprintf("Arx kernel: task_selftest FAIL: ExecuteTask(taskA) failed\n");
        goto cleanup;
    }

    kprintf("Arx kernel: task_selftest returned to BSP after task chain, step=%d\n", g_task_test_ctx.step);

    if (g_task_test_ctx.step != 3)
    {
        fails++;
        kprintf("Arx kernel: task_selftest FAIL: expected step=3, got %d\n", g_task_test_ctx.step);
    }
    else
    {
        passes++;
    }

cleanup:
    if (surrogateTask != nullptr)
    {
        manager->SetRunningTask(cpuId, originalRunningTask);
    }

    if (taskA != nullptr)
    {
        manager->FreeTask(taskA);
    }
    if (taskB != nullptr)
    {
        manager->FreeTask(taskB);
    }

    if (surrogateTask != nullptr)
    {
        manager->FreeTask(surrogateTask);
    }

done:
    selftest_case_end("task_selftest", passes, fails);
}
