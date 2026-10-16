#pragma once

#include <arch/arch.h>
#include <stdint.h>

// Control waits use both a five-second deadline and a clock-free work bound.
// This cannot interrupt a CPU that never returns from a context switch.
class PollBudget
{
public:
    PollBudget() : Polls(0), Start(0), Clock(arch_time_read_monotonic(&Start))
    {
    }

    bool Expired()
    {
        if (++Polls >= 100000000ULL)
        {
            return true;
        }

        if (Clock && (Polls & 1023) == 0)
        {
            uint64_t now;

            if (arch_time_read_monotonic(&now) && now >= Start && now - Start >= 5000000000ULL)
            {
                return true;
            }
        }

        return false;
    }

private:
    uint64_t Polls;
    uint64_t Start;
    bool     Clock;
};
