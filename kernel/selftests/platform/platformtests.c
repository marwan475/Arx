#include "platformtests.h"
#include <selftests/selftests.h>

void run_platform_selftest_group(void)
{
    selftest_group_begin("platform");
    run_datastructures_selftests();
    run_memory_selftests();
    run_klib_selftests();
    run_platform_invariants();
    selftest_group_end("platform");
}
