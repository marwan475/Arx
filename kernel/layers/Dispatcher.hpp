#pragma once
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
class Dispatcher
{
public:
    Dispatcher();
    ~Dispatcher();
    void StartKernel();

private:
    ResourceLayerFactory* resourceLayerFactory;
    LogicLayerFactory* logicLayerFactory;
};
