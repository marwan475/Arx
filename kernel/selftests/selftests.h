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

void                      selftest_reset_context(void);
const selftest_context_t* selftest_get_context(void);

void selftest_group_begin(const char* group_name);
void selftest_group_end(const char* group_name);

void selftest_case_begin(const char* test_name);
void selftest_case_end(const char* test_name, unsigned long long passes, unsigned long long failures);
void selftest_print_summary(void);

void platform_selftests(void);

void run_datastructures_selftests(void);
void run_memory_selftests(void);
void run_klib_selftests(void);
void run_task_selftests(void* resourceLayerCaps);
void run_process_selftests(void* resourceLayerCaps, void* logicLayerCaps);
void run_vfs_selftests(void* logicLayerCaps);
void run_poststart_vfs_selftests(void* resourceLayerCaps, void* logicLayerCaps);
void run_poststart_elf_selftests(void* resourceLayerCaps, void* logicLayerCaps);
void run_smp_scheduler_selftest(void* logicLayerCaps, void* resourceLayerCaps, unsigned long long cpuId, unsigned long long* outPasses, unsigned long long* outFails);

void smp_selftests(void);
void smp_selftests_wait_for_all_cpus(void);
void smp_selftests_get_totals(unsigned long long* out_passes, unsigned long long* out_fails, unsigned long long* out_finished_cpus);

void resourcelayer_selftests(void* resourceLayerCaps);
void logiclayer_selftests(void* resourceLayerCaps, void* logicLayerCaps);
void poststartkerneltests(void* resourceLayerCaps, void* logicLayerCaps);

#ifdef __cplusplus
}
#endif

#endif
