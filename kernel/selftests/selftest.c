// Ai generated testing

#include <klib/klib.h>
#include <selftests/selftests.h>

static selftest_context_t g_selftest_ctx;

#define SELFTEST_MAX_FAILED_CASES 64
static const char*        g_failed_case_names[SELFTEST_MAX_FAILED_CASES];
static unsigned long long g_failed_case_count = 0;

#define SELFTEST_MAX_CASES 128
#define SELFTEST_DETAIL_MAX 256
typedef struct selftest_case_result
{
    const char*        test_name;
    unsigned long long passes;
    unsigned long long failures;
} selftest_case_result_t;

static selftest_case_result_t g_case_results[SELFTEST_MAX_CASES];
static unsigned long long     g_case_result_count = 0;
static unsigned long long     g_current_case_idx  = (unsigned long long) -1;
static char                   g_case_fail_details[SELFTEST_MAX_CASES][SELFTEST_DETAIL_MAX];

static void selftest_append_detail(char* dst, const char* src)
{
    if (dst == 0 || src == 0)
    {
        return;
    }

    const size_t dstLen = strlen(dst);
    if (dstLen >= (SELFTEST_DETAIL_MAX - 1))
    {
        return;
    }

    size_t srcIndex = 0;
    size_t dstIndex = dstLen;
    while (src[srcIndex] != '\0' && dstIndex < (SELFTEST_DETAIL_MAX - 1))
    {
        dst[dstIndex++] = src[srcIndex++];
    }
    dst[dstIndex] = '\0';
}

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
    g_case_result_count = 0;
    g_current_case_idx  = (unsigned long long) -1;

    memset(g_case_results, 0, sizeof(g_case_results));
    memset(g_case_fail_details, 0, sizeof(g_case_fail_details));
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
        snapshot->group_name                = group_name;
        snapshot->tests_ran                 = g_selftest_ctx.tests_ran;
        snapshot->tests_passed              = g_selftest_ctx.tests_passed;
        snapshot->tests_failed              = g_selftest_ctx.tests_failed;
        g_group_stack_depth++;
    }

    kprintf("\n[SELFTEST] GROUP START  %s\n", group_name);
    KDEBUG("\n========== SELFTEST GROUP START: %s =========\n", group_name);
}

void selftest_group_end(const char* group_name)
{
    unsigned long long group_ran    = 0;
    unsigned long long group_passed = 0;
    unsigned long long group_failed = 0;

    if (group_name == 0)
    {
        group_name = "unknown";
    }

    if (g_group_stack_depth > 0)
    {
        const selftest_group_snapshot_t* snapshot = &g_group_stack[g_group_stack_depth - 1];
        group_ran                                 = g_selftest_ctx.tests_ran - snapshot->tests_ran;
        group_passed                              = g_selftest_ctx.tests_passed - snapshot->tests_passed;
        group_failed                              = g_selftest_ctx.tests_failed - snapshot->tests_failed;
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

    if (g_case_result_count < SELFTEST_MAX_CASES)
    {
        const unsigned long long idx = g_case_result_count;
        g_case_results[idx].test_name = test_name;
        g_case_results[idx].passes    = 0;
        g_case_results[idx].failures  = 0;
        g_case_fail_details[idx][0]   = '\0';
        g_current_case_idx            = idx;
        g_case_result_count++;
    }
    else
    {
        g_current_case_idx = (unsigned long long) -1;
    }
}

void selftest_record_failure_detail(const char* detail)
{
    if (detail == 0 || detail[0] == '\0')
    {
        return;
    }

    if (g_current_case_idx == (unsigned long long) -1 || g_current_case_idx >= SELFTEST_MAX_CASES)
    {
        return;
    }

    char* dst = g_case_fail_details[g_current_case_idx];
    if (dst[0] != '\0')
    {
        selftest_append_detail(dst, "; ");
    }

    selftest_append_detail(dst, detail);
}

void selftest_case_end(const char* test_name, unsigned long long passes, unsigned long long failures)
{
    if (test_name == 0)
    {
        test_name = "unknown";
    }

    const char* result = failures == 0 ? "PASS" : "FAIL";

    if (g_current_case_idx != (unsigned long long) -1 && g_current_case_idx < SELFTEST_MAX_CASES)
    {
        g_case_results[g_current_case_idx].test_name = test_name;
        g_case_results[g_current_case_idx].passes    = passes;
        g_case_results[g_current_case_idx].failures  = failures;

        if (failures != 0 && g_case_fail_details[g_current_case_idx][0] == '\0')
        {
            selftest_append_detail(g_case_fail_details[g_current_case_idx], "failure count incremented with no explicit detail");
        }
    }

    (void) __atomic_fetch_add(&g_selftest_ctx.tests_ran, 1ULL, __ATOMIC_RELAXED);
    if (failures == 0)
    {
        (void) __atomic_fetch_add(&g_selftest_ctx.tests_passed, 1ULL, __ATOMIC_RELAXED);
    }
    else
    {
        (void) __atomic_fetch_add(&g_selftest_ctx.tests_failed, 1ULL, __ATOMIC_RELAXED);
        unsigned long long slot = __atomic_fetch_add(&g_failed_case_count, 1ULL, __ATOMIC_RELAXED);
        if (slot < SELFTEST_MAX_FAILED_CASES)
        {
            g_failed_case_names[slot] = test_name;
        }
    }

    kprintf("[SELFTEST] RESULT %s | checks(pass=%llu fail=%llu) => %s\n", test_name, passes, failures, result);
    KDEBUG("[selftest] case end: %s | checks(pass=%llu fail=%llu) => %s\n", test_name, passes, failures, result);

    g_current_case_idx = (unsigned long long) -1;
}

void selftest_print_summary(void)
{
    const unsigned long long testsRan    = __atomic_load_n(&g_selftest_ctx.tests_ran, __ATOMIC_RELAXED);
    const unsigned long long testsPassed = __atomic_load_n(&g_selftest_ctx.tests_passed, __ATOMIC_RELAXED);
    const unsigned long long testsFailed = __atomic_load_n(&g_selftest_ctx.tests_failed, __ATOMIC_RELAXED);
    const unsigned long long failedCount = __atomic_load_n(&g_failed_case_count, __ATOMIC_RELAXED);
    const unsigned long long failedPrint = failedCount < SELFTEST_MAX_FAILED_CASES ? failedCount : SELFTEST_MAX_FAILED_CASES;

    kprintf("\n========================================\n");
    kprintf(" Arx kernel selftest summary\n");
    kprintf("========================================\n");
    kprintf(" tests ran   : %llu\n", testsRan);
    kprintf(" tests passed: %llu\n", testsPassed);
    kprintf(" tests failed: %llu\n", testsFailed);
    kprintf(" overall     : %s\n", testsFailed == 0 ? "PASS" : "FAIL");
    if (failedPrint > 0)
    {
        kprintf(" failed cases:\n");
        for (unsigned long long i = 0; i < failedPrint; i++)
        {
            kprintf("  - %s\n", g_failed_case_names[i]);
        }
    }

    if (g_case_result_count > 0)
    {
        kprintf(" case details:\n");
        for (unsigned long long i = 0; i < g_case_result_count; i++)
        {
            const char* name   = g_case_results[i].test_name ? g_case_results[i].test_name : "unknown";
            const char* result = g_case_results[i].failures == 0 ? "PASS" : "FAIL";
            kprintf("  - %s | pass=%llu fail=%llu => %s\n", name, g_case_results[i].passes, g_case_results[i].failures, result);
            if (g_case_results[i].failures != 0)
            {
                const char* detail = g_case_fail_details[i][0] != '\0' ? g_case_fail_details[i] : "no detail";
                kprintf("    detail: %s\n", detail);
            }
        }
    }
    kprintf("========================================\n\n");

    KDEBUG("[selftest] summary: ran=%llu pass=%llu fail=%llu overall=%s\n", testsRan, testsPassed, testsFailed, testsFailed == 0 ? "PASS" : "FAIL");
    if (failedPrint > 0)
    {
        for (unsigned long long i = 0; i < failedPrint; i++)
        {
            KDEBUG("[selftest] failed case: %s\n", g_failed_case_names[i]);
        }
    }

    if (g_case_result_count > 0)
    {
        for (unsigned long long i = 0; i < g_case_result_count; i++)
        {
            const char* name   = g_case_results[i].test_name ? g_case_results[i].test_name : "unknown";
            const char* result = g_case_results[i].failures == 0 ? "PASS" : "FAIL";
            KDEBUG("[selftest] case detail: %s | pass=%llu fail=%llu => %s\n", name, g_case_results[i].passes, g_case_results[i].failures, result);
            if (g_case_results[i].failures != 0)
            {
                const char* detail = g_case_fail_details[i][0] != '\0' ? g_case_fail_details[i] : "no detail";
                KDEBUG("[selftest] case detail reason: %s\n", detail);
            }
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
    run_poststart_elf_selftests(resourceLayerCaps, logicLayerCaps);
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
