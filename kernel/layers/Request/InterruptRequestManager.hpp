#pragma once

#include <boot/boot.h>
#include <stdint.h>

enum interrupt_request_number
{
    INTERRUPT_REQUEST_SCHEDULE = 1,
};

#ifdef __cplusplus
class InterruptRequestManager
{
public:
    InterruptRequestManager();
    void HandleInterruptRequest(uint64_t requestNumber);

private:
    void HandleScheduleRequest();

    static constexpr uint64_t SCHEDULE_TICK_INTERVAL = 5;
    uint64_t                  ScheduleTickCounters[BOOT_SMP_MAX_CPUS];
};
#endif