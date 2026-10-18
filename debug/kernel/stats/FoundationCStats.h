#ifndef ARX_FOUNDATION_C_STATS_H
#define ARX_FOUNDATION_C_STATS_H

#include "../DebugRecords.h"

#if DEBUG

typedef enum foundation_phase
{
    FOUNDATION_EARLY,
    FOUNDATION_READY
} foundation_phase_t;

typedef struct foundation_c_stats
{
    uint64_t          calls;
    foundation_phase_t phase;
    struct
    {
        uint64_t bytes;
        uint32_t samples[3];
    } usage;
    const void* opaque;
} foundation_c_stats_t;

#ifdef __cplusplus
extern "C"
{
#endif

    extern foundation_c_stats_t arx_foundation_c_stats;
    void arx_foundation_c_update(void);

#ifdef __cplusplus
}
#endif

#endif
#endif
