#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/AcpiManager.hpp"

#include "layers/Resource/DmaManager.hpp"
#include "layers/Resource/DeviceManager.hpp"
#include "layers/Resource/InitRamFileSystemManager.hpp"
#include "layers/Resource/PciManager.hpp"
#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
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
        delete ResourceLayerExportCaps->dmaManager;
        delete ResourceLayerExportCaps->pciManager;
        delete ResourceLayerExportCaps->acpiManager;
        delete ResourceLayerExportCaps->deviceManager;
        delete ResourceLayerExportCaps->timeManager;
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
    ResourceLayerExportCaps->dmaManager               = new DmaManager(ResourceLayerExportCaps->physicalMemoryManager, ResourceLayerExportCaps->virtualMemoryManager, &platform.dma_ops);
    ResourceLayerExportCaps->timeManager              = new TimeManager();
    ResourceLayerExportCaps->deviceManager            = new DeviceManager();
    ResourceLayerExportCaps->acpiManager              = new AcpiManager(ResourceLayerExportCaps->deviceManager);
    ResourceLayerExportCaps->pciManager               = new PciManager(&platform_pci_ops, ResourceLayerExportCaps->deviceManager);
    ResourceLayerExportCaps->initRamFileSystemManager = new InitRamFileSystemManager(platform.initramfs_size, platform.initramfs_address);
    ResourceFileSystemExport                          = new ResourceFileSystem(ResourceLayerExportCaps->initRamFileSystemManager);
    ResourceLayerExportCaps->fileSystemCaps           = ResourceFileSystemExport->GetCaps();

    return ResourceLayerExportCaps;
}

ResourceLayerCaps* ResourceLayerFactory::GetCaps() const
{
    return ResourceLayerExportCaps;
}
