#include "layers/Dispatcher.hpp"

#include "layers/Resource/ResourceLayerFactory.hpp"

Dispatcher::Dispatcher()
{
    resourceLayerFactory = new ResourceLayerFactory();
}

Dispatcher::~Dispatcher()
{
    delete resourceLayerFactory;
}

void Dispatcher::StartKernel()
{
    ResourceLayerCaps* ResourceLayerImportCaps = resourceLayerFactory->Create();
}


