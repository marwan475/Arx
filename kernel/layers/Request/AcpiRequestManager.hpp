#pragma once

#include <stdint.h>

struct ResourceLayerCaps;
struct LogicLayerCaps;

// Kernel-service ingress only. Platform reaches scheduling through Dispatcher
// and this Request handler; firmware and hardware remain entirely in Platform.
class AcpiRequestManager
{
public:
    static uint64_t ThreadId(ResourceLayerCaps* resources);
    static bool CanWait(ResourceLayerCaps* resources, LogicLayerCaps* logic);
    static bool Wait(ResourceLayerCaps* resources, LogicLayerCaps* logic, const uint64_t* sequence, uint64_t expected, uint64_t deadline);
    static void Wake(LogicLayerCaps* logic, const uint64_t* sequence);
    static bool CreateWorker(ResourceLayerCaps* resources, LogicLayerCaps* logic, void (*entry)(void*), void* argument);
};
