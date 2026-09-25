#ifndef SELFTESTS_H
#define SELFTESTS_H

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
