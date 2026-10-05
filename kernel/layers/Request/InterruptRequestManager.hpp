#pragma once

#include <stdint.h>

enum interrupt_request_number
{
    INTERRUPT_REQUEST_SCHEDULE = 1,
};

#ifdef __cplusplus
class InterruptRequestManager
{
public:
    void HandleInterruptRequest(uint64_t requestNumber);
};
#endif