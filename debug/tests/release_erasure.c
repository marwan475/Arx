#include <debug/kernel/DebugRecords.h>

int arx_release_side_effects;

ARX_DEBUG_ONLY(static uint64_t debugOnlyRecord;)

int arx_release_probe(void)
{
    ARX_DEBUG_ADD(debugOnlyRecord, ++arx_release_side_effects);
    ARX_DEBUG_STORE(debugOnlyRecord, ++arx_release_side_effects);
    ARX_DEBUG_RUN(arx_release_side_effects++);
    ARX_DEBUG_ONLY(arx_release_side_effects++;)

    return arx_release_side_effects;
}

int main(void)
{
    return arx_release_probe();
}
