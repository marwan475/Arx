#include "layers/Logic/LogicLayerFactory.hpp"

#include "layers/Logic/CredentialManager.hpp"
#include "layers/Logic/ElfMapper.hpp"
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/SignalManager.hpp"
#include "layers/Logic/SynchronizationManager.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

LogicLayerFactory::LogicLayerFactory()
{
    LogicLayerExportCaps = nullptr;
}

LogicLayerFactory::~LogicLayerFactory()
{
    if (LogicLayerExportCaps != nullptr)
    {
        delete LogicLayerExportCaps->signalManager;
        delete LogicLayerExportCaps->synchronizationManager;
        delete LogicLayerExportCaps->eventManager;
        delete LogicLayerExportCaps->credentialManager;
        delete LogicLayerExportCaps->elfMapper;
        delete LogicLayerExportCaps->virtualFileSystem;
        delete LogicLayerExportCaps->scheduler;
        delete LogicLayerExportCaps;
        LogicLayerExportCaps = nullptr;
    }
}

LogicLayerCaps* LogicLayerFactory::Create(ResourceLayerCaps* resourceLayerCaps)
{
    if (LogicLayerExportCaps != nullptr)
    {
        return LogicLayerExportCaps;
    }

    if (resourceLayerCaps == nullptr)
    {
        return nullptr;
    }

    LogicLayerExportCaps                         = new LogicLayerCaps();
    LogicLayerExportCaps->scheduler              = new Scheduler(resourceLayerCaps);
    LogicLayerExportCaps->virtualFileSystem      = new VirtualFileSystem(resourceLayerCaps);
    LogicLayerExportCaps->elfMapper              = new ElfMapper(resourceLayerCaps);
    LogicLayerExportCaps->credentialManager      = new CredentialManager(resourceLayerCaps);
    LogicLayerExportCaps->eventManager           = new EventManager(resourceLayerCaps, LogicLayerExportCaps->scheduler, LogicLayerExportCaps->virtualFileSystem);
    LogicLayerExportCaps->synchronizationManager = new SynchronizationManager(resourceLayerCaps, LogicLayerExportCaps->scheduler);
    LogicLayerExportCaps->signalManager          = new SignalManager(resourceLayerCaps, LogicLayerExportCaps->scheduler);

    return LogicLayerExportCaps;
}

LogicLayerCaps* LogicLayerFactory::GetCaps() const
{
    return LogicLayerExportCaps;
}
