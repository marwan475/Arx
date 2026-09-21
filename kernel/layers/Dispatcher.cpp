#include "layers/Dispatcher.hpp"

#include "layers/Resource/ResourceLayerFactory.hpp"

extern "C"
{
#include <selftests/selftests.h>
}

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

    resourcelayer_selftests((void*) ResourceLayerImportCaps);
}


