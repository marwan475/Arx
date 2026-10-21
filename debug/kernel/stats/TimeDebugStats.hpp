#pragma once
#include "SchedulingDebug.hpp"
#include "TaskDebugStats.hpp"
#include "TimePlatformDebug.h"

#if DEBUG
class TimeManager;

// arx.time/1. count=0 capacity=64. clocks reads immutable manager configuration
// plus independent successful normal-path sample high-water (ns, monotonic),
// never backend callbacks. sampleReads=0 means no sample, including value=0.
// timers scans allocated ProcessDebugStats slots in physical order, validating
// process generations. Per-process timerDebugGroup brackets timerLock updates;
// CPU accounting is independently atomic, excludes live uncharged execution.
// realTimerDeadline/Interval are MONOTONIC ns. cpuTimer[0] is USER CPU ns,
// cpuTimer[1] is saturating USER+KERNEL CPU ns, zero deadline means disarmed.
// realTimerExpired means awaiting restart, not necessarily signal delivery.
// setTimer/setAlarm count returned Linux results, independently of timerCommits:
// a timer may commit before SetTimer returns EFAULT copying its old value.
// timerCommits includes disarms; realExpirations/cpuExpirations count deadline
// transitions even if a resulting signal is ignored. realRestarts counts re-arms,
// not restart probes. Samples use atomic max plus separate atomic counts, never
// a claimed coherent timestamp/counter pair. Per-process group counters persist
// across quiescent slot reuse; process generation defines the current occupant.
// Backend function/context addresses identify configuration only. platform is
// null for deterministic/custom backends. No clock contexts may be followed.
struct TimeDebugStats
{
    TimeManager* source;
    const ProcessDebugStats* processStats;
    const TaskDebugStats* taskStats;
    const arx_time_platform_debug_t* platform;
    arx_debug_group_t group;
    uint64_t clockAvailable;
    uint64_t realtimeAvailable;
    uint64_t realtimeSeconds;
    uint64_t resolutionNanoseconds;
    uint64_t backendInitialize;
    uint64_t backendRead;
    uint64_t backendContext;
    uint64_t sampleNanoseconds;
    uint64_t sampleReads;
    uint64_t sampleFailures;
    DebugResultStats setTimer;
    DebugResultStats setAlarm;
    uint64_t timerCommits;
    uint64_t realExpirations;
    uint64_t cpuExpirations;
    uint64_t realRestarts;
};
#endif
