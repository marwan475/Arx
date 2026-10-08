#pragma once

#include <boot/boot.h>
#include <cstddef>
#include <stdint.h>

struct ResourceLayerCaps;

class Scheduler
{
public:
    explicit Scheduler(ResourceLayerCaps* resourceLayerCaps);
    ~Scheduler() = default;

    void ActivateScheduling();
    bool IsSchedulingActive() const;

    bool ScheduleProcess(uint64_t processId);
    bool EnqueueProcess(uint8_t cpuId, uint64_t processId);
    bool RunNextReadyProcess(uint8_t cpuId);
    bool KillProcess(uint64_t processId, int32_t exitStatus);

private:
    void InitializeBspProcessAndTask();

    static constexpr size_t READY_QUEUE_CAPACITY = 64;

    struct ready_queue_t
    {
        uint64_t processIds[READY_QUEUE_CAPACITY];
        size_t   head;
        size_t   tail;
        size_t   count;
    };

    bool RemoveProcessFromQueue(ready_queue_t* queue, uint64_t processId);

    ready_queue_t ReadyQueues[BOOT_SMP_MAX_CPUS];
    bool          SchedulingActive[BOOT_SMP_MAX_CPUS];

    ResourceLayerCaps* ResourceLayerImportCaps;
};
