#pragma once

#include <stdint.h>

struct task_t;

struct scheduling_attributes_t
{
    uint32_t size;
    uint32_t policy;
    uint64_t flags;
    int32_t nice;
    uint32_t priority;
    uint64_t runtime;
    uint64_t deadline;
    uint64_t period;
    uint32_t utilizationMin;
    uint32_t utilizationMax;
};
static_assert(sizeof(scheduling_attributes_t) == 56, "Linux sched_attr ABI");

struct task_scheduling_t
{
    uint32_t policy;
    uint32_t priority;
    uint32_t effectivePolicy;
    uint32_t effectivePriority;
    bool resetOnFork;
    int16_t reservationCpu;
    uint64_t readyOrder;
    uint64_t roundRobinRemaining;
    uint64_t deadlineRuntime;
    uint64_t deadlinePeriod;
    uint64_t deadlineRemaining;
    uint64_t deadlineAbsolute;
    uint64_t deadlineRelease;
    task_t* deadlineDonor;
};

constexpr uint32_t SCHEDULING_OTHER = 0;
constexpr uint32_t SCHEDULING_FIFO = 1;
constexpr uint32_t SCHEDULING_RR = 2;
constexpr uint32_t SCHEDULING_DEADLINE = 6;
constexpr uint64_t SCHEDULING_RR_QUANTUM = 10000000ULL;
constexpr uint64_t SCHEDULING_RT_WINDOW = 100000000ULL;
constexpr uint64_t SCHEDULING_UTILIZATION_SCALE = 1000000ULL;
constexpr uint64_t SCHEDULING_DEADLINE_CAPACITY = 800000ULL;
