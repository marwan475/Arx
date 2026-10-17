#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Configuration is immutable after publication; elapsed time is shared by CPUs.
// Narrow counters require a sample at least once per full hardware period.
typedef struct clock_source
{
    uint64_t (*read)(void* context);
    void*    context;
    uint64_t mask;
    uint32_t numerator;
    uint32_t denominator;
    uint64_t origin;
    uint64_t elapsed;
    uint64_t nanoseconds;
} clock_source_t;

static inline bool clock_ticks_to_nanoseconds(uint64_t ticks, uint32_t numerator, uint32_t denominator, uint64_t* result)
{
    if (numerator == 0 || denominator == 0 || result == NULL)
    {
        return false;
    }

    const uint64_t whole    = ticks / denominator;
    const uint64_t fraction = (ticks % denominator) * numerator / denominator;

    if (whole > (UINT64_MAX - fraction) / numerator)
    {
        return false;
    }

    *result = whole * numerator + fraction;
    return true;
}

// Probe before publication; failed candidates never become visible to readers.
static inline bool clock_source_start(clock_source_t* source)
{
    if (source->read == NULL || source->mask == 0 || (source->mask & (source->mask + 1)) != 0 || source->numerator == 0 || source->denominator == 0)
    {
        return false;
    }

    const uint64_t initial = source->read(source->context) & source->mask;

    for (unsigned attempt = 0; attempt < 100000; ++attempt)
    {
        const uint64_t sample = source->read(source->context) & source->mask;

        if (sample != initial)
        {
            source->origin      = sample;
            source->elapsed     = 0;
            source->nanoseconds = 0;
            return true;
        }
    }

    return false;
}

static inline clock_source_t* clock_source_select(clock_source_t* sources, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (clock_source_start(&sources[index]))
        {
            return &sources[index];
        }
    }

    return NULL;
}

static inline bool clock_source_read(clock_source_t* source, uint64_t* result)
{
    if (source == NULL || result == NULL)
    {
        return false;
    }

    uint64_t elapsed;

    for (;;)
    {
        uint64_t previous = __atomic_load_n(&source->elapsed, __ATOMIC_ACQUIRE);
        // A failed CAS must obtain a new hardware sample, otherwise a sample
        // older than the winner's baseline would be interpreted as a wrap.
        const uint64_t sample = source->read(source->context) & source->mask;
        const uint64_t delta  = (sample - (source->origin + previous)) & source->mask;

        if (delta > UINT64_MAX - previous)
        {
            return false;
        }

        elapsed = previous + delta;

        if (__atomic_compare_exchange_n(&source->elapsed, &previous, elapsed, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            break;
        }
    }

    uint64_t converted;

    if (!clock_ticks_to_nanoseconds(elapsed, source->numerator, source->denominator, &converted))
    {
        return false;
    }

    uint64_t published = __atomic_load_n(&source->nanoseconds, __ATOMIC_ACQUIRE);

    while (published < converted && !__atomic_compare_exchange_n(&source->nanoseconds, &published, converted, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
    }

    *result = published > converted ? published : converted;
    return true;
}
