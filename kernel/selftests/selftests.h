#ifndef SELFTESTS_H
#define SELFTESTS_H

void platform_selftests(void);

void run_datastructures_selftests(void);
void run_memory_selftests(void);
void run_klib_selftests(void);
void run_task_selftests(void* resourceLayerCaps);

void resourcelayer_selftests(void* resourceLayerCaps);

#endif
