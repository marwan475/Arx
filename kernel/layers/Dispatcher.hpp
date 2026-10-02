#pragma once
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
class Dispatcher
{
public:
    Dispatcher();
    ~Dispatcher();
    void               StartKernel();
    ResourceLayerCaps* GetResourceLayerCaps() const;
    LogicLayerCaps*    GetLogicLayerCaps() const;

private:
    ResourceLayerFactory* resourceLayerFactory;
    LogicLayerFactory*    logicLayerFactory;
};
