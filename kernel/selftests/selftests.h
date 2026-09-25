#ifndef SELFTESTS_H
#define SELFTESTS_H

typedef struct selftest_context
{
	unsigned long long tests_ran;
	unsigned long long tests_passed;
	unsigned long long tests_failed;
} selftest_context_t;

void selftest_reset_context(void);
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

void resourcelayer_selftests(void* resourceLayerCaps);
void logiclayer_selftests(void* resourceLayerCaps, void* logicLayerCaps);
void poststartkerneltests(void* resourceLayerCaps, void* logicLayerCaps);

#endif
