#include "stats/FoundationCStats.h"

#if DEBUG

foundation_c_stats_t arx_foundation_c_stats;

void arx_foundation_c_update(void)
{
    ARX_DEBUG_ADD(arx_foundation_c_stats.calls, 1);
    ARX_DEBUG_ADD(arx_foundation_c_stats.usage.bytes, 64);
}

#endif
