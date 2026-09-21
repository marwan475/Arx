// Ai generated testing
// Not thread safe

#include <selftests/selftests.h>

void resourcelayer_selftests(void* resourceLayerCaps)
{
    run_task_selftests(resourceLayerCaps);
}

void platform_selftests(void)
{
    run_datastructures_selftests();
    run_memory_selftests();
    run_klib_selftests();
}
