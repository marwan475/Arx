#pragma once

#include <stddef.h>
#include <stdint.h>

struct process_t;

// Linux LP64 wire layouts. Never dereference these through a user pointer.
struct time_timespec_t
{
    int64_t seconds;
    int64_t nanoseconds;
};
struct time_timeval_t
{
    int64_t seconds;
    int64_t microseconds;
};
struct time_itimerval_t
{
    time_timeval_t interval;
    time_timeval_t value;
};
static_assert(sizeof(time_timespec_t) == 16 && sizeof(time_timeval_t) == 16 && sizeof(time_itimerval_t) == 32, "Linux time ABI");

class TimeManager
{
public:
    TimeManager();
    uint64_t ReadClock(int clock, time_timespec_t* value);
    uint64_t ReadResolution(int clock, time_timespec_t* value);
    uint64_t CalculateSleepDeadline(int clock, bool absolute, const time_timespec_t& request, uint64_t* monotonicDeadline);
    uint64_t GetTimer(int which, uint64_t destination);
    uint64_t SetTimer(int which, uint64_t source, uint64_t previous);
    uint64_t CancelAlarm();
    uint64_t SetAlarm(uint32_t seconds);
    // Caller retains the process. Expiration publishes SIGALRM before returning.
    bool     ExpireRealTimer(process_t* process, uint64_t now);
    void     RestartRealTimer(process_t* process);
    bool     ExpireCpuTimers(process_t* process);

private:
    // Caller holds the process timer lock.
    void     SnapshotTimer(process_t* process, int which, uint64_t now, time_itimerval_t* value);
    uint64_t RealtimeSeconds;
    uint64_t ResolutionNanoseconds;
    bool     ClockAvailable;
    bool     RealtimeAvailable;
};
