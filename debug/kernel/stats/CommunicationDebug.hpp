#pragma once
#include "ProcessDebugStats.hpp"

#if DEBUG
// Each scope counts a provider operation, never retries inside that operation.
// Nonnegative results (including zero/EOF) succeed; shortReturns counts positive
// results smaller than a known requested byte count. returnedBytes is the API
// number, which can exceed copiedBytes under MSG_TRUNC. Unreturned/retired calls
// remain inFlight. All counters are atomic modular uint64_t observations.
struct CommunicationOperationStats
{
    uint64_t attempts;
    uint64_t successes;
    uint64_t failures;
    uint64_t inFlight;
    uint64_t shortReturns;
    uint64_t zeroReturns;
    uint64_t returnedBytes;
};

class CommunicationOperation
{
public:
    CommunicationOperation(CommunicationOperationStats& stats, bool transfer = false, uint64_t requested = 0)
        : Stats(stats), Transfer(transfer), Requested(requested), Finished(false)
    {
        ARX_DEBUG_ADD(Stats.attempts, 1);
        ARX_DEBUG_ADD(Stats.inFlight, 1);
    }

    void Request(uint64_t requested)
    {
        Requested = requested;
    }

    template<typename T> T Return(T value)
    {
        if (!Finished)
        {
            if (static_cast<int64_t>(value) < 0)
            {
                ARX_DEBUG_ADD(Stats.failures, 1);
            }
            else
            {
                ARX_DEBUG_ADD(Stats.successes, 1);

                if (Transfer)
                {
                    const uint64_t bytes = static_cast<uint64_t>(value);
                    ARX_DEBUG_ADD(Stats.returnedBytes, bytes);

                    if (bytes == 0)
                    {
                        ARX_DEBUG_ADD(Stats.zeroReturns, 1);
                    }
                    else if (bytes < Requested)
                    {
                        ARX_DEBUG_ADD(Stats.shortReturns, 1);
                    }
                }
            }

            ARX_DEBUG_ADD(Stats.inFlight, UINT64_MAX);
            Finished = true;
        }

        return value;
    }

private:
    CommunicationOperationStats& Stats;
    bool Transfer;
    uint64_t Requested;
    bool Finished;
};

#define ARX_COMM_RETURN(value) debugOperation.Return(value)
#else
#define ARX_COMM_RETURN(value) (value)
#endif
