#pragma once

#include "selftests.h"

#include <stdint.h>

// Initialized explicitly by the platform phase before any recording. Names are
// borrowed for the boot lifetime. Case/group recording belongs to the BSP;
// SMP workers collect their own results and publish an aggregate separately.
class SelfTestManager
{
public:
    static SelfTestManager& Get();

    void Initialize();
    const selftest_context_t* GetContext() const;

    void GroupBegin(const char* group_name);
    void GroupEnd(const char* group_name);
    void CaseBegin(const char* test_name);
    void CaseEnd(const char* test_name, unsigned long long passes, unsigned long long failures,
                 selftest_count_kind_t count_kind = SELFTEST_COUNT_PASSES);
    void RecordFailureDetail(const char* detail);
    void PrintSummary();
    void CaseFinish(const char* name, unsigned long long passes, unsigned long long failures, selftest_status_t status);
    void Finalize();

    [[noreturn]] void AbortUnsafe(const char* reason);

    void RunPlatformTests();
    void RunResourceLayerTests(void* resourceLayerCaps);
    void RunLogicLayerTests(void* resourceLayerCaps, void* logicLayerCaps);

    // Publish the mount result before any per-CPU post-init tasks are queued.
    void SetRootFileSystemMounted(bool mounted);
    void RunPostStartTests(void* resourceLayerCaps, void* logicLayerCaps);
    void RunRequestTests(void* requestLayerCaps);

    // Called on each CPU in its post-init task. Only the BSP records cases.
    void RunPostInitTests();

private:
    SelfTestManager() = default;
    SelfTestManager(const SelfTestManager&) = delete;
    SelfTestManager& operator=(const SelfTestManager&) = delete;

    static SelfTestManager instance;

    static constexpr unsigned long long MaxCases       = 256;
    static constexpr unsigned long long DetailMax      = 256;
    static constexpr unsigned long long MaxFailedCases = 256;
    static constexpr unsigned long long GroupStackMax  = 16;

    struct CaseResult
    {
    const char*           test_name;
        unsigned long long    passes;
        unsigned long long    failures;
        selftest_count_kind_t count_kind;
        selftest_status_t     status;
        bool                  detail_truncated;
    };

    struct GroupSnapshot
    {
        const char*        group_name;
        unsigned long long tests_ran;
        unsigned long long tests_passed;
        unsigned long long tests_failed;
        unsigned long long skipped;
        unsigned long long blocked;
    };

    static void AppendDetail(char* dst, const char* src);
    selftest_context_t context;
    const char*        failedCaseNames[MaxFailedCases];
    unsigned long long failedCaseCount;
    CaseResult         caseResults[MaxCases];
    unsigned long long caseResultCount;
    unsigned long long currentCaseIndex;
    char               caseFailDetails[MaxCases][DetailMax];
    GroupSnapshot      groupStack[GroupStackMax];
    unsigned long long groupStackDepth;
    bool               rootFileSystemMounted;
    unsigned long long skipped;
    unsigned long long blocked;
    unsigned long long omittedCases;
    const char*        activeCaseName;
    uint64_t           caseStart;
    bool               hasCaseClock;
    bool               finalized;
};
