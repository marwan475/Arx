// Ai generated testing
// Not thread safe

#include <selftests/selftests.h>

void resourcelayer_selftests(void* resourceLayerCaps)
{
    run_task_selftests(resourceLayerCaps);
}

void logiclayer_selftests(void* resourceLayerCaps, void* logicLayerCaps)
{
    run_process_selftests(resourceLayerCaps, logicLayerCaps);
    run_vfs_selftests(logicLayerCaps);
}

void poststartkerneltests(void* resourceLayerCaps, void* logicLayerCaps)
{
    (void)resourceLayerCaps;
    run_poststart_vfs_selftests(logicLayerCaps);
}

void platform_selftests(void)
{
    run_datastructures_selftests();
    run_memory_selftests();
    run_klib_selftests();
}
