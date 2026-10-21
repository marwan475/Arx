#include "SelfTestManager.hpp"
#include "layers/Dispatcher.hpp"
#include "platform/platformtests.h"
#include "resource/resourcetests.hpp"
#include "logic/logictests.hpp"
#include "request/requesttests.hpp"
#include "poststartkernel/poststartkerneltests.hpp"

#include <arch/arch.h>
#include <klib/klib.h>
#include <platform.h>

// Trivial static storage: early C tests do not depend on global constructors.
SelfTestManager SelfTestManager::instance;

static const char* status_name(selftest_status_t status)
{
    switch (status)
    {
        case SELFTEST_PASS:
            return "PASS";
        case SELFTEST_FAIL:
            return "FAIL";
        case SELFTEST_SKIP:
            return "SKIP";
        case SELFTEST_BLOCKED:
            return "BLOCKED";
    }

    return "INVALID";
}

SelfTestManager& SelfTestManager::Get()
{
    return instance;
}

void SelfTestManager::AppendDetail(char* dst, const char* src)
{
    if (dst == 0 || src == 0)
    {
        return;
    }

    const size_t dstLen = strlen(dst);

    if (dstLen >= (DetailMax - 1))
    {
        return;
    }

    size_t srcIndex = 0;
    size_t dstIndex = dstLen;

    while (src[srcIndex] != '\0' && dstIndex < (DetailMax - 1))
    {
        dst[dstIndex++] = src[srcIndex++];
    }

    dst[dstIndex] = '\0';
}

void SelfTestManager::Initialize(void)
{
    context.tests_ran    = 0;
    context.tests_passed = 0;
    context.tests_failed = 0;
    groupStackDepth      = 0;
    failedCaseCount      = 0;
    caseResultCount      = 0;
    currentCaseIndex     = (unsigned long long) -1;
    skipped              = blocked = omittedCases = 0;
    activeCaseName       = nullptr;
    finalized            = false;
    memset(caseResults, 0, sizeof(caseResults));
    memset(caseFailDetails, 0, sizeof(caseFailDetails));
}

const selftest_context_t* SelfTestManager::GetContext(void) const
{
    return &context;
}

void SelfTestManager::GroupBegin(const char* group_name)
{
    if (group_name == 0)
    {
        group_name = "unknown";
    }

    if (groupStackDepth < GroupStackMax)
    {
        GroupSnapshot* snapshot = &groupStack[groupStackDepth];
        snapshot->group_name    = group_name;
        snapshot->tests_ran     = context.tests_ran;
        snapshot->tests_passed  = context.tests_passed;
        snapshot->tests_failed  = context.tests_failed;
        snapshot->skipped       = skipped;
        snapshot->blocked       = blocked;
        groupStackDepth++;
    }

    kprintf("\n[SELFTEST] GROUP START  %s\n", group_name);
    KDEBUG("\n========== SELFTEST GROUP START: %s =========\n", group_name);
}

void SelfTestManager::GroupEnd(const char* group_name)
{
    unsigned long long group_ran     = 0;
    unsigned long long group_passed  = 0;
    unsigned long long group_failed  = 0;
    unsigned long long group_skipped = 0;
    unsigned long long group_blocked = 0;

    if (group_name == 0)
    {
        group_name = "unknown";
    }

    if (groupStackDepth > 0)
    {
        const GroupSnapshot* snapshot = &groupStack[groupStackDepth - 1];
        group_ran                     = context.tests_ran - snapshot->tests_ran;
        group_passed                  = context.tests_passed - snapshot->tests_passed;
        group_failed                  = context.tests_failed - snapshot->tests_failed;
        group_skipped                 = skipped - snapshot->skipped;
        group_blocked                 = blocked - snapshot->blocked;
        groupStackDepth--;
    }

    kprintf("[SELFTEST] GROUP END %s | ran=%llu pass=%llu fail=%llu skip=%llu blocked=%llu\n", group_name, group_ran, group_passed, group_failed, group_skipped, group_blocked);
    KDEBUG("========== SELFTEST GROUP END: %s | ran=%llu pass=%llu fail=%llu =========\n\n", group_name, group_ran, group_passed, group_failed);
}

void SelfTestManager::CaseBegin(const char* test_name)
{
    if (test_name == 0)
    {
        test_name = "unknown";
    }

    kprintf("\n[SELFTEST] START  %s\n", test_name);
    KDEBUG("[selftest] case start: %s\n", test_name);
    activeCaseName = test_name;
    hasCaseClock   = arch_time_read_monotonic(&caseStart);

    if (caseResultCount < MaxCases)
    {
        const unsigned long long idx = caseResultCount;
        caseResults[idx].test_name   = test_name;
        caseResults[idx].passes      = 0;
        caseResults[idx].failures    = 0;
        caseResults[idx].count_kind  = SELFTEST_COUNT_PASSES;
        caseResults[idx].status      = SELFTEST_BLOCKED;
        caseFailDetails[idx][0]      = '\0';
        currentCaseIndex             = idx;
        caseResultCount++;
    }
    else
    {
        currentCaseIndex = (unsigned long long) -1;

        if (++omittedCases == 1)
        {
            kprintf("[SELFTEST] retained case capacity reached; all further results still stream\n");
        }
    }
}

void SelfTestManager::RecordFailureDetail(const char* detail)
{
    if (detail == 0 || detail[0] == '\0')
    {
        return;
    }

    const char* name = activeCaseName != nullptr ? activeCaseName : "unknown";
    error_logger("selftest %s: %s", name != NULL ? name : "unknown", detail);

    if (currentCaseIndex == (unsigned long long) -1 || currentCaseIndex >= MaxCases)
    {
        return;
    }

    char* dst = caseFailDetails[currentCaseIndex];

    if (strlen(dst) + strlen(detail) + (dst[0] != '\0' ? 2 : 0) >= DetailMax)
    {
        caseResults[currentCaseIndex].detail_truncated = true;
    }

    if (dst[0] != '\0')
    {
        AppendDetail(dst, "; ");
    }

    AppendDetail(dst, detail);
}

void SelfTestManager::CaseEnd(const char* test_name, unsigned long long passes, unsigned long long failures, selftest_count_kind_t count_kind)
{
    if (test_name == 0)
    {
        test_name = "unknown";
    }

    const char* result = failures == 0 ? "PASS" : "FAIL";

    if (currentCaseIndex != (unsigned long long) -1 && currentCaseIndex < MaxCases)
    {
        caseResults[currentCaseIndex].test_name  = test_name;
        caseResults[currentCaseIndex].passes     = passes;
        caseResults[currentCaseIndex].failures   = failures;
        caseResults[currentCaseIndex].count_kind = count_kind;
        caseResults[currentCaseIndex].status     = failures ? SELFTEST_FAIL : SELFTEST_PASS;

        if (failures != 0 && caseFailDetails[currentCaseIndex][0] == '\0')
        {
            error_logger("selftest %s: %llu failures without explicit detail", test_name, failures);
            AppendDetail(caseFailDetails[currentCaseIndex], "failure count incremented with no explicit detail");
        }

    } (void) __atomic_fetch_add(&context.tests_ran, 1ULL, __ATOMIC_RELAXED);

    if (failures == 0)
    {
        (void) __atomic_fetch_add(&context.tests_passed, 1ULL, __ATOMIC_RELAXED);
    }
    else
    {
        (void) __atomic_fetch_add(&context.tests_failed, 1ULL, __ATOMIC_RELAXED);
        unsigned long long slot = __atomic_fetch_add(&failedCaseCount, 1ULL, __ATOMIC_RELAXED);

        if (slot < MaxFailedCases)
        {
            failedCaseNames[slot] = test_name;
        }
    }

    kprintf("[SELFTEST] RESULT %s | checks(pass=%llu fail=%llu) => %s\n", test_name, passes, failures, result);
    KDEBUG("[selftest] case end: %s | checks(pass=%llu fail=%llu) => %s\n", test_name, passes, failures, result);
    uint64_t end = 0;

    if (hasCaseClock && arch_time_read_monotonic(&end) && end >= caseStart)
    {
        kprintf("[SELFTEST] duration %s: %llu ns\n", test_name, (unsigned long long) (end - caseStart));
    }
    else
    {
        kprintf("[SELFTEST] duration %s: unavailable\n", test_name);
    }

    currentCaseIndex = (unsigned long long) -1;
    activeCaseName   = nullptr;
}

void SelfTestManager::CaseFinish(const char* name, unsigned long long passes, unsigned long long failures, selftest_status_t status)
{
    if (failures || status == SELFTEST_FAIL || status == SELFTEST_PASS)
    {
        CaseEnd(name, passes, failures ? failures : (status == SELFTEST_FAIL ? 1 : 0));
        return;
    }

    if (status == SELFTEST_SKIP)
    {
        ++skipped;
    }
    else
    {
        ++blocked;
    }

    if (currentCaseIndex < caseResultCount)
    {
        caseResults[currentCaseIndex].status   = status;
        caseResults[currentCaseIndex].passes   = passes;
        caseResults[currentCaseIndex].failures = 0;
    }

    kprintf("[SELFTEST] RESULT %s | checks(pass=%llu fail=0) => %s\n", name, passes, status_name(status));
    uint64_t end = 0;

    if (hasCaseClock && arch_time_read_monotonic(&end) && end >= caseStart)
    {
        kprintf("[SELFTEST] duration %s: %llu ns\n", name, (unsigned long long) (end - caseStart));
    }
    else
    {
        kprintf("[SELFTEST] duration %s: unavailable\n", name);
    }

    currentCaseIndex = (unsigned long long) -1;
    activeCaseName   = nullptr;
}

void SelfTestManager::Finalize()
{
    if (__atomic_exchange_n(&finalized, true, __ATOMIC_ACQ_REL))
    {
        return;
    }

    PrintSummary();
    error_logger_print();
}

[[noreturn]] void SelfTestManager::AbortUnsafe(const char* reason)
{
    error_logger("selftest unsafe initialization: %s", reason);

    if (activeCaseName != nullptr)
    {
        RecordFailureDetail(reason);
        CaseEnd(activeCaseName, 0, 1);
    }

    CaseBegin("init/remaining_groups");
    CaseFinish("init/remaining_groups", 0, 0, SELFTEST_BLOCKED);
    Finalize();
    arch_disable_interrupts();

    for (;;)
    {
        arch_halt();
    }
}

void SelfTestManager::PrintSummary(void)
{
    kprintf("[SELFTEST] nonexecuted: skip=%llu blocked=%llu; retained=%llu omitted=%llu; failed names omitted=%llu\n", skipped, blocked, caseResultCount, omittedCases, failedCaseCount > MaxFailedCases ? failedCaseCount - MaxFailedCases : 0);
    const unsigned long long testsRan    = __atomic_load_n(&context.tests_ran, __ATOMIC_RELAXED);
    const unsigned long long testsPassed = __atomic_load_n(&context.tests_passed, __ATOMIC_RELAXED);
    const unsigned long long testsFailed = __atomic_load_n(&context.tests_failed, __ATOMIC_RELAXED);
    const unsigned long long failedCount = __atomic_load_n(&failedCaseCount, __ATOMIC_RELAXED);
    const unsigned long long failedPrint = failedCount < MaxFailedCases ? failedCount : MaxFailedCases;
    kprintf("\n========================================\n");
    kprintf(" Arx kernel selftest summary\n");
    kprintf("========================================\n");
    kprintf(" tests ran   : %llu\n", testsRan);
    kprintf(" tests passed: %llu\n", testsPassed);
    kprintf(" tests failed: %llu\n", testsFailed);
    kprintf(" overall     : %s\n", testsFailed != 0 ? "FAIL" : (blocked != 0 ? "INCOMPLETE" : "PASS"));

    if (failedPrint > 0)
    {
        kprintf(" failed cases:\n");

        for (unsigned long long i = 0; i < failedPrint; i++)
        {
            kprintf("  - %s\n", failedCaseNames[i]);
        }
    }

    if (caseResultCount > 0)
    {
        kprintf(" case details:\n");

        for (unsigned long long i = 0; i < caseResultCount; i++)
        {
            const char* name   = caseResults[i].test_name ? caseResults[i].test_name : "unknown";
            const char* result = status_name(caseResults[i].status);

            if (caseResults[i].count_kind == SELFTEST_COUNT_IMPLEMENTED)
            {
                kprintf("  - %s | implemented=%llu fail=%llu => %s\n", name, caseResults[i].passes, caseResults[i].failures, result);
            }
            else
            {
                kprintf("  - %s | pass=%llu fail=%llu => %s\n", name, caseResults[i].passes, caseResults[i].failures, result);
            }

            if (caseResults[i].failures != 0)
            {
                const char* detail = caseFailDetails[i][0] != '\0' ? caseFailDetails[i] : "no detail";
                kprintf("    detail: %s\n", detail);

                if (caseResults[i].detail_truncated)
                {
                    kprintf("    [retained detail preview truncated; complete failures streamed above]\n");
                }
            }
        }
    }

    kprintf("========================================\n\n");
    KDEBUG("[selftest] summary: ran=%llu pass=%llu fail=%llu overall=%s\n", testsRan, testsPassed, testsFailed, testsFailed != 0 ? "FAIL" : (blocked != 0 ? "INCOMPLETE" : "PASS"));

    if (failedPrint > 0)
    {
        for (unsigned long long i = 0; i < failedPrint; i++)
        {
            KDEBUG("[selftest] failed case: %s\n", failedCaseNames[i]);
        }
    }

    if (caseResultCount > 0)
    {
        for (unsigned long long i = 0; i < caseResultCount; i++)
        {
            const char* name   = caseResults[i].test_name ? caseResults[i].test_name : "unknown";
            const char* result = status_name(caseResults[i].status);

            if (caseResults[i].count_kind == SELFTEST_COUNT_IMPLEMENTED)
            {
                KDEBUG("[selftest] case detail: %s | implemented=%llu fail=%llu => %s\n", name, caseResults[i].passes, caseResults[i].failures, result);
            }
            else
            {
                KDEBUG("[selftest] case detail: %s | pass=%llu fail=%llu => %s\n", name, caseResults[i].passes, caseResults[i].failures, result);
            }

            if (caseResults[i].failures != 0)
            {
                const char* detail = caseFailDetails[i][0] != '\0' ? caseFailDetails[i] : "no detail";
                KDEBUG("[selftest] case detail reason: %s\n", detail);
            }
        }
    }
}

void SelfTestManager::RunResourceLayerTests(void* resourceLayerCaps)
{
    run_resource_selftest_group(static_cast<ResourceLayerCaps*>(resourceLayerCaps));
}

void SelfTestManager::RunLogicLayerTests(void* resourceLayerCaps, void* logicLayerCaps)
{
    run_logic_selftest_group(static_cast<ResourceLayerCaps*>(resourceLayerCaps), static_cast<LogicLayerCaps*>(logicLayerCaps));
}

void SelfTestManager::SetRootFileSystemMounted(bool mounted)
{
    __atomic_store_n(&rootFileSystemMounted, mounted, __ATOMIC_RELEASE);
}

void SelfTestManager::RunPciDiscoveryTests(void* resourceLayerCaps)
{
    run_pci_discovery_selftest_group(static_cast<ResourceLayerCaps*>(resourceLayerCaps));
}

void SelfTestManager::RunPostStartTests(void* resourceLayerCaps, void* logicLayerCaps)
{
    run_poststartkernel_selftest_group(resourceLayerCaps, logicLayerCaps, __atomic_load_n(&rootFileSystemMounted, __ATOMIC_ACQUIRE));
}

void SelfTestManager::RunRequestTests(void* requestLayerCaps)
{
    run_request_selftest_group(requestLayerCaps);
}

void SelfTestManager::RunPlatformTests(void)
{
    Initialize();
    run_platform_selftest_group();
}

void SelfTestManager::RunPostInitTests()
{
    Dispatcher* dispatcher   = (Dispatcher*) platform.dispacher;
    void*       resourceCaps = dispatcher != nullptr ? dispatcher->GetResourceLayerCaps() : nullptr;
    void*       logicCaps    = dispatcher != nullptr ? dispatcher->GetLogicLayerCaps() : nullptr;
    RunPostStartTests(resourceCaps, logicCaps);

    if (arch_cpu_id() == platform.bsp_id)
    {
        if (dispatcher != nullptr)
        {
            RunRequestTests((void*) dispatcher->GetRequestLayerCaps());
        }

        Finalize();
    }
}

// C tests keep their existing ABI; all framework state belongs to the manager.
extern "C" void selftest_reset_context(void)
{
    SelfTestManager::Get().Initialize();
}

extern "C" const selftest_context_t* selftest_get_context(void)
{
    return SelfTestManager::Get().GetContext();
}

extern "C" void selftest_group_begin(const char* name)
{
    SelfTestManager::Get().GroupBegin(name);
}

extern "C" void selftest_group_end(const char* name)
{
    SelfTestManager::Get().GroupEnd(name);
}

extern "C" void selftest_case_begin(const char* name)
{
    SelfTestManager::Get().CaseBegin(name);
}

extern "C" void selftest_case_end(const char* name, unsigned long long passes, unsigned long long failures)
{
    SelfTestManager::Get().CaseEnd(name, passes, failures);
}

extern "C" void selftest_case_end_with_count_kind(const char* name, unsigned long long passes, unsigned long long failures, selftest_count_kind_t kind)
{
    SelfTestManager::Get().CaseEnd(name, passes, failures, kind);
}

extern "C" void selftest_record_failure_detail(const char* detail)
{
    SelfTestManager::Get().RecordFailureDetail(detail);
}

extern "C" void selftest_print_summary(void)
{
    SelfTestManager::Get().PrintSummary();
}

extern "C" void platform_selftests(void)
{
    SelfTestManager::Get().RunPlatformTests();
}

extern "C" void resourcelayer_selftests(void* resource)
{
    SelfTestManager::Get().RunResourceLayerTests(resource);
}

extern "C" void logiclayer_selftests(void* resource, void* logic)
{
    SelfTestManager::Get().RunLogicLayerTests(resource, logic);
}

extern "C" void poststartkerneltests(void* resource, void* logic)
{
    SelfTestManager::Get().RunPostStartTests(resource, logic);
}

extern "C" void selftest_case_finish(const char* name, unsigned long long passes, unsigned long long failures, selftest_status_t status)
{
    SelfTestManager::Get().CaseFinish(name, passes, failures, status);
}
