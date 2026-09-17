#pragma once
#include "layers/Resource/ResourceLayerFactory.hpp"
class Dispatcher
{
public:
    Dispatcher();
    ~Dispatcher();
    void StartKernel();

private:
    ResourceLayerFactory* resourceLayerFactory;
};
