#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void run_platform_selftest_group(void);
void run_datastructures_selftests(void);
void run_memory_selftests(void);
void run_klib_selftests(void);
void run_platform_invariants(void);

#ifdef __cplusplus
}
#endif
