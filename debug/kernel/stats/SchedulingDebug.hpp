#pragma once
#include "ProcessDebugStats.hpp"

#if DEBUG

// API results and state transitions are separate. A nonnegative Linux result
// (including wake=0) is success. inFlight means no return was recorded, including
// suspended calls and calls whose task retires without returning. Counts wrap u64.
struct DebugResultStats
{
    uint64_t attempts;
    uint64_t returnedSuccess;
    uint64_t returnedFailure;
    uint64_t inFlight;
};

class DebugResultOperation
{
public:
    explicit DebugResultOperation(DebugResultStats& stats) : Stats(stats)
    {
        ARX_DEBUG_ADD(Stats.attempts, 1);
        ARX_DEBUG_ADD(Stats.inFlight, 1);
    }

    uint64_t Return(uint64_t result)
    {
        if (static_cast<int64_t>(result) < 0)
        {
            ARX_DEBUG_ADD(Stats.returnedFailure, 1);
        }
        else
        {
            ARX_DEBUG_ADD(Stats.returnedSuccess, 1);
        }

        ARX_DEBUG_ADD(Stats.inFlight, UINT64_MAX);
        return result;
    }

private:
    DebugResultStats& Stats;
};

#define ARX_WAIT_RESULT(operation, value) operation.Return(value)
#else
#define ARX_WAIT_RESULT(operation, value) (value)
#endif
