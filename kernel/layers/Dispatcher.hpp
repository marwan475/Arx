#pragma once

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

#include <debug/kernel/DebugManager.hpp>

class Dispatcher
{
public:
    Dispatcher();
    ~Dispatcher();
    void               StartKernel();
    ResourceLayerCaps* GetResourceLayerCaps() const;
    LogicLayerCaps*    GetLogicLayerCaps() const;
    RequestLayerCaps*  GetRequestLayerCaps() const;
    uint64_t           DispatchSyscall(const arch_syscall_frame_t* frame) const;
    void               DispatchInterruptRequest(uint64_t interruptNumber) const;

private:
#if DEBUG
    DebugManager debugManager;
#endif

    ResourceLayerFactory* resourceLayerFactory;
    LogicLayerFactory*    logicLayerFactory;
    RequestLayerFactory*  requestLayerFactory;
};
