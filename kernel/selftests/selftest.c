// Ai generated testing
// Not thread safe

#include <klib/klib.h>
#include <selftests/selftests.h>

static selftest_context_t g_selftest_ctx;

#define SELFTEST_MAX_FAILED_CASES 64
static const char*         g_failed_case_names[SELFTEST_MAX_FAILED_CASES];
static unsigned long long  g_failed_case_count = 0;

typedef struct selftest_group_snapshot
{
    const char*        group_name;
    unsigned long long tests_ran;
    unsigned long long tests_passed;
    unsigned long long tests_failed;
} selftest_group_snapshot_t;

#define SELFTEST_GROUP_STACK_MAX 16
static selftest_group_snapshot_t g_group_stack[SELFTEST_GROUP_STACK_MAX];
static unsigned long long        g_group_stack_depth = 0;

void selftest_reset_context(void)
{
    g_selftest_ctx.tests_ran    = 0;
    g_selftest_ctx.tests_passed = 0;
    g_selftest_ctx.tests_failed = 0;

    g_group_stack_depth = 0;
    g_failed_case_count = 0;
}

const selftest_context_t* selftest_get_context(void)
{
    return &g_selftest_ctx;
}

void selftest_group_begin(const char* group_name)
{
    if (group_name == 0)
    {
        group_name = "unknown";
    }

    if (g_group_stack_depth < SELFTEST_GROUP_STACK_MAX)
    {
        selftest_group_snapshot_t* snapshot = &g_group_stack[g_group_stack_depth];
        snapshot->group_name   = group_name;
        snapshot->tests_ran    = g_selftest_ctx.tests_ran;
        snapshot->tests_passed = g_selftest_ctx.tests_passed;
        snapshot->tests_failed = g_selftest_ctx.tests_failed;
        g_group_stack_depth++;
    }

    kprintf("\n[SELFTEST] GROUP START  %s\n", group_name);
    KDEBUG("\n========== SELFTEST GROUP START: %s =========\n", group_name);
}

void selftest_group_end(const char* group_name)
{
    unsigned long long group_ran = 0;
    unsigned long long group_passed = 0;
    unsigned long long group_failed = 0;

    if (group_name == 0)
    {
        group_name = "unknown";
    }

    if (g_group_stack_depth > 0)
    {
        const selftest_group_snapshot_t* snapshot = &g_group_stack[g_group_stack_depth - 1];
        group_ran    = g_selftest_ctx.tests_ran - snapshot->tests_ran;
        group_passed = g_selftest_ctx.tests_passed - snapshot->tests_passed;
        group_failed = g_selftest_ctx.tests_failed - snapshot->tests_failed;
        g_group_stack_depth--;
    }

    kprintf("[SELFTEST] GROUP END    %s | ran=%llu pass=%llu fail=%llu\n", group_name, group_ran, group_passed, group_failed);
    KDEBUG("========== SELFTEST GROUP END: %s | ran=%llu pass=%llu fail=%llu =========\n\n", group_name, group_ran, group_passed, group_failed);
}

void selftest_case_begin(const char* test_name)
{
    if (test_name == 0)
    {
        test_name = "unknown";
    }

    kprintf("\n[SELFTEST] START  %s\n", test_name);
    KDEBUG("[selftest] case start: %s\n", test_name);
}

void selftest_case_end(const char* test_name, unsigned long long passes, unsigned long long failures)
{
    if (test_name == 0)
    {
        test_name = "unknown";
    }

    const char* result = failures == 0 ? "PASS" : "FAIL";

    g_selftest_ctx.tests_ran++;
    if (failures == 0)
    {
        g_selftest_ctx.tests_passed++;
    }
    else
    {
        g_selftest_ctx.tests_failed++;
        if (g_failed_case_count < SELFTEST_MAX_FAILED_CASES)
        {
            g_failed_case_names[g_failed_case_count++] = test_name;
        }
    }

    kprintf("[SELFTEST] RESULT %s | checks(pass=%llu fail=%llu) => %s\n", test_name, passes, failures, result);
    KDEBUG("[selftest] case end: %s | checks(pass=%llu fail=%llu) => %s\n", test_name, passes, failures, result);
}

void selftest_print_summary(void)
{
    kprintf("\n========================================\n");
    kprintf(" Arx kernel selftest summary\n");
    kprintf("========================================\n");
    kprintf(" tests ran   : %llu\n", g_selftest_ctx.tests_ran);
    kprintf(" tests passed: %llu\n", g_selftest_ctx.tests_passed);
    kprintf(" tests failed: %llu\n", g_selftest_ctx.tests_failed);
    kprintf(" overall     : %s\n", g_selftest_ctx.tests_failed == 0 ? "PASS" : "FAIL");
    if (g_failed_case_count > 0)
    {
        kprintf(" failed cases:\n");
        for (unsigned long long i = 0; i < g_failed_case_count; i++)
        {
            kprintf("  - %s\n", g_failed_case_names[i]);
        }
    }
    kprintf("========================================\n\n");

    KDEBUG("[selftest] summary: ran=%llu pass=%llu fail=%llu overall=%s\n", g_selftest_ctx.tests_ran, g_selftest_ctx.tests_passed, g_selftest_ctx.tests_failed,
           g_selftest_ctx.tests_failed == 0 ? "PASS" : "FAIL");
    if (g_failed_case_count > 0)
    {
        for (unsigned long long i = 0; i < g_failed_case_count; i++)
        {
            KDEBUG("[selftest] failed case: %s\n", g_failed_case_names[i]);
        }
    }
}

void resourcelayer_selftests(void* resourceLayerCaps)
{
    selftest_group_begin("resourcelayer");
    run_task_selftests(resourceLayerCaps);
    selftest_group_end("resourcelayer");
}

void logiclayer_selftests(void* resourceLayerCaps, void* logicLayerCaps)
{
    selftest_group_begin("logiclayer");
    run_process_selftests(resourceLayerCaps, logicLayerCaps);
    run_vfs_selftests(logicLayerCaps);
    selftest_group_end("logiclayer");
}

void poststartkerneltests(void* resourceLayerCaps, void* logicLayerCaps)
{
    selftest_group_begin("poststart");
    run_poststart_vfs_selftests(resourceLayerCaps, logicLayerCaps);
    selftest_group_end("poststart");
}

void platform_selftests(void)
{
    selftest_reset_context();
    selftest_group_begin("platform");

    run_datastructures_selftests();
    run_memory_selftests();
    run_klib_selftests();

    selftest_group_end("platform");
}
