#pragma once

#include <stdint.h>

struct ResourceLayerCaps;

class Scheduler
{
public:
    explicit Scheduler(ResourceLayerCaps* resourceLayerCaps);
    ~Scheduler() = default;

    bool ScheduleProcess(uint64_t processId);

private:
    void InitializeBspProcessAndTask();

    ResourceLayerCaps* ResourceLayerImportCaps;
};
