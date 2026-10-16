#ifndef SELFTESTS_H
#define SELFTESTS_H

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct selftest_context
    {
        unsigned long long tests_ran;
        unsigned long long tests_passed;
        unsigned long long tests_failed;
    } selftest_context_t;

    typedef enum selftest_count_kind
    {
        SELFTEST_COUNT_PASSES,
        SELFTEST_COUNT_IMPLEMENTED
    } selftest_count_kind_t;

    typedef enum selftest_status
    {
        SELFTEST_PASS,
        SELFTEST_FAIL,
        SELFTEST_SKIP,
        SELFTEST_BLOCKED
    } selftest_status_t;

    void selftest_case_finish(const char* name, unsigned long long passes, unsigned long long failures, selftest_status_t status);

    void                      selftest_reset_context(void);
    const selftest_context_t* selftest_get_context(void);

    void selftest_group_begin(const char* group_name);
    void selftest_group_end(const char* group_name);

    void selftest_case_begin(const char* test_name);
    void selftest_case_end(const char* test_name, unsigned long long passes, unsigned long long failures);
    void selftest_case_end_with_count_kind(const char* test_name, unsigned long long passes, unsigned long long failures, selftest_count_kind_t count_kind);
    void selftest_record_failure_detail(const char* detail);
    void selftest_print_summary(void);

    void platform_selftests(void);

    void resourcelayer_selftests(void* resourceLayerCaps);
    void logiclayer_selftests(void* resourceLayerCaps, void* logicLayerCaps);
    void poststartkerneltests(void* resourceLayerCaps, void* logicLayerCaps);

#ifdef __cplusplus
}

#endif

#endif
