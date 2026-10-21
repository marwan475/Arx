#pragma once
#include <debug/kernel/DebugRecords.h>
#include <boot/boot.h>
#include <klib/clock.h>

#if DEBUG
#ifdef __cplusplus
extern "C"
{
#endif

// Architecture writes only at normal initialization/calibration sites. source
// points to RAM clock_source_t; NEVER call read or dereference context. sourceId
// 0=unavailable, 1=HPET, 2=ACPI PM timer. Configuration immutable after release
// publication. elapsed and nanoseconds are independent atomic runtime high-water
// samples, not a coherent pair or current time. No age/overdue claims are valid.
// LAPIC ticks are per 10ms; zero=unavailable; CPU slots are bounded by boot max.
typedef struct arx_time_platform_debug
{
    const clock_source_t* source;
    uint64_t sourceId;
    uint64_t lapicTicks[BOOT_SMP_MAX_CPUS];
    arx_debug_group_t group;
} arx_time_platform_debug_t;

extern arx_time_platform_debug_t arx_time_platform_debug;
#ifdef __cplusplus
}
#endif
#endif
