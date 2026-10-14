#include "layers/Resource/ResourceLayerFactory.hpp"

#include "layers/Resource/InitRamFileSystemManager.hpp"
#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ProcessMemoryManager.hpp"
#include "layers/Resource/ResourceFileSystem.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/TimeManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <platform.h>

ResourceLayerFactory::ResourceLayerFactory()
{
    ResourceLayerExportCaps  = nullptr;
    ResourceFileSystemExport = nullptr;
}

ResourceLayerFactory::~ResourceLayerFactory()
{
    if (ResourceLayerExportCaps != nullptr)
    {
        delete ResourceLayerExportCaps->timeManager;
        delete ResourceLayerExportCaps->processMemoryManager;
        delete ResourceLayerExportCaps->physicalMemoryManager;
        delete ResourceLayerExportCaps->processManager;
        delete ResourceLayerExportCaps->taskManager;
        delete ResourceLayerExportCaps->virtualMemoryManager;
        delete ResourceLayerExportCaps->initRamFileSystemManager;
        delete ResourceFileSystemExport;
        ResourceFileSystemExport = nullptr;
        delete ResourceLayerExportCaps;
        ResourceLayerExportCaps = nullptr;
    }
}

ResourceLayerCaps* ResourceLayerFactory::Create()
{
    if (ResourceLayerExportCaps != nullptr)
    {
        return ResourceLayerExportCaps;
    }

    ResourceLayerExportCaps                           = new ResourceLayerCaps();
    ResourceLayerExportCaps->physicalMemoryManager    = new PhysicalMemoryManager();
    ResourceLayerExportCaps->processManager           = new ProcessManager();
    ResourceLayerExportCaps->taskManager              = new TaskManager();
    ResourceLayerExportCaps->virtualMemoryManager     = new VirtualMemoryManager();
    ResourceLayerExportCaps->processMemoryManager     = new ProcessMemoryManager(ResourceLayerExportCaps->processManager, ResourceLayerExportCaps->virtualMemoryManager);
    ResourceLayerExportCaps->timeManager              = new TimeManager();
    ResourceLayerExportCaps->initRamFileSystemManager = new InitRamFileSystemManager(platform.initramfs_size, platform.initramfs_address);
    ResourceFileSystemExport                          = new ResourceFileSystem(ResourceLayerExportCaps->initRamFileSystemManager);
    ResourceLayerExportCaps->fileSystemCaps           = ResourceFileSystemExport->GetCaps();

    return ResourceLayerExportCaps;
}

ResourceLayerCaps* ResourceLayerFactory::GetCaps() const
{
    return ResourceLayerExportCaps;
}
