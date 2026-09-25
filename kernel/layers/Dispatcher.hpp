#pragma once
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
class Dispatcher
{
public:
    Dispatcher();
    ~Dispatcher();
    void StartKernel();
    ResourceLayerCaps* GetResourceLayerCaps() const;
    LogicLayerCaps* GetLogicLayerCaps() const;

private:
    ResourceLayerFactory* resourceLayerFactory;
    LogicLayerFactory* logicLayerFactory;
};
