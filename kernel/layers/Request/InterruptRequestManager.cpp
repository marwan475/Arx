#include "layers/Request/InterruptRequestManager.hpp"

void InterruptRequestManager::HandleInterruptRequest(uint64_t requestNumber)
{
    switch (requestNumber)
    {
        case INTERRUPT_REQUEST_SCHEDULE:
            return;
        default:
            return;
    }
}