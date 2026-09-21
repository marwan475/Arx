// Basic task context-switch selftest between two task entry functions.

#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

extern "C"
{
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
    size_t passes = 0;
    size_t fails  = 0;
    TaskManager* manager = nullptr;
    task_t*      taskA   = nullptr;
    task_t*      taskB   = nullptr;
    task_t*      bspTask = nullptr;

    kprintf("Arx kernel: task_selftest start\n");
    kprintf("Arx kernel: task_selftest start\n");

    ResourceLayerCaps* caps = static_cast<ResourceLayerCaps*>(resourceLayerCaps);
    if (caps == nullptr || caps->taskManager == nullptr)
    {
        fails++;
        kprintf("Arx kernel: task_selftest FAIL: missing resource layer task manager\n");
        goto done;
    }

    manager = caps->taskManager;

    bspTask = manager->GetCurrentTask();
    if (bspTask == nullptr)
    {
        fails++;
        kprintf("Arx kernel: task_selftest FAIL: missing BSP current task\n");
        goto done;
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
    if (taskA != nullptr)
    {
        manager->FreeTask(taskA);
    }
    if (taskB != nullptr)
    {
        manager->FreeTask(taskB);
    }

done:
    kprintf("Arx kernel: task_selftest summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) fails);
    kprintf("Arx kernel: task_selftest RESULT=%s\n", fails == 0 ? "PASS" : "FAIL");
    kprintf("Arx kernel: task_selftest RESULT=%s\n", fails == 0 ? "PASS" : "FAIL");
}
