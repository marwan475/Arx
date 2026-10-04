#pragma once

#include <boot/boot.h>
#include <klib/spinlock.h>
#include <stdint.h>

struct ResourceLayerCaps;

class Scheduler
{
public:
    explicit Scheduler(ResourceLayerCaps* resourceLayerCaps);
    ~Scheduler() = default;

    bool ScheduleProcess(uint64_t processId);
    bool EnqueueProcess(uint8_t cpuId, uint64_t processId);
    bool RunNextReadyProcess(uint8_t cpuId);

private:
    void InitializeBspProcessAndTask();

    static constexpr size_t READY_QUEUE_CAPACITY = 64;

    struct ready_queue_t
    {
        uint64_t   processIds[READY_QUEUE_CAPACITY];
        size_t     head;
        size_t     tail;
        size_t     count;
        spinlock_t lock;
    };

    ready_queue_t ReadyQueues[BOOT_SMP_MAX_CPUS];

    ResourceLayerCaps* ResourceLayerImportCaps;
};
