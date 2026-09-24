#include "layers/Dispatcher.hpp"

#include "layers/Resource/ResourceLayerFactory.hpp"

extern "C"
{
#include <selftests/selftests.h>
}

Dispatcher::Dispatcher()
{
    resourceLayerFactory = new ResourceLayerFactory();
    logicLayerFactory    = new LogicLayerFactory();
}

Dispatcher::~Dispatcher()
{
    delete resourceLayerFactory;
    delete logicLayerFactory;
}

void Dispatcher::StartKernel()
{
    ResourceLayerCaps* ResourceLayerImportCaps = resourceLayerFactory->Create();

    resourcelayer_selftests((void*) ResourceLayerImportCaps);

    LogicLayerCaps* LogicLayerImportCaps = logicLayerFactory->Create(ResourceLayerImportCaps);

    logiclayer_selftests((void*) ResourceLayerImportCaps, (void*) LogicLayerImportCaps);
}

ResourceLayerCaps* Dispatcher::GetResourceLayerCaps() const
{
    return resourceLayerFactory != nullptr ? resourceLayerFactory->GetCaps() : nullptr;
}

LogicLayerCaps* Dispatcher::GetLogicLayerCaps() const
{
    return logicLayerFactory != nullptr ? logicLayerFactory->GetCaps() : nullptr;
}


