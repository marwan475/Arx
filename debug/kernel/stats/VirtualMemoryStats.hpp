#pragma once

#include "ProcessDebugStats.hpp"
#include "VirtualMemoryDebugStats.h"

#if DEBUG

// Manager-lifetime operation counts. Nested APIs describe distinct layers;
// successes are completed calls, not unique physical pages or bytes.
struct VirtualMemoryStats
{
    arx_debug_group_t group;
    DebugOperationStats createSpace;
    DebugOperationStats cloneSpace;
    DebugOperationStats allocatePage;
    DebugOperationStats releasePage;
    DebugOperationStats createBacking;
    DebugOperationStats mapBacking;
    uint64_t spacesReclaimed;
    uint64_t destroyRequests;
    uint64_t destroyDeferred;
};

struct ElfDebugStats
{
    arx_debug_group_t group;
    DebugOperationStats parse;
    DebugOperationStats validate;
    DebugOperationStats loadExecutable;
    DebugOperationStats loadLibrary;
    DebugOperationStats mapImage;
};

// Classify return values once while preserving the API result. kind 0 is
// boolean/pointer, 1 signed status, 2 Linux unsigned result (errno -4095..-1).
class DebugMemoryOperation
{
public:
    DebugMemoryOperation(DebugOperationStats& stats, unsigned kind)
        : Operation(stats), Kind(kind)
    {
    }

    template<typename T> T Result(T value)
    {
        bool success = false;
        if (Kind == 0)
        {
            success = value != 0;
        }
        else if (Kind == 1)
        {
            success = (int64_t) value >= 0;
        }
        else
        {
            success = (uint64_t) value < (uint64_t) -4095;
        }

        Operation.Finish(success ? 0 : 2);
        return value;
    }

private:
    DebugOperation Operation;
    unsigned Kind;
};

#define ARX_MEMORY_SCOPE(stats, kind) \
    DebugMemoryOperation debugMemory(stats, kind)
#define ARX_MEMORY_RETURN(value) debugMemory.Result(value)

#else
#define ARX_MEMORY_SCOPE(stats, kind) do { } while (0)
#define ARX_MEMORY_RETURN(value) (value)
#endif
