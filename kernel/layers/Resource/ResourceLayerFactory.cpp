#include "layers/Resource/ResourceLayerFactory.hpp"

#include "layers/Resource/InitRamFileSystemManager.hpp"
#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"
#include <platform.h>

ResourceLayerFactory::ResourceLayerFactory()
{
	ResourceLayerExportCaps = nullptr;
}

ResourceLayerFactory::~ResourceLayerFactory()
{
	if (ResourceLayerExportCaps != nullptr)
	{
		delete ResourceLayerExportCaps->physicalMemoryManager;
		delete ResourceLayerExportCaps->processManager;
		delete ResourceLayerExportCaps->taskManager;
		delete ResourceLayerExportCaps->virtualMemoryManager;
		delete ResourceLayerExportCaps->initRamFileSystemManager;
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

	ResourceLayerExportCaps                        = new ResourceLayerCaps();
	ResourceLayerExportCaps->physicalMemoryManager = new PhysicalMemoryManager();
	ResourceLayerExportCaps->processManager        = new ProcessManager();
	ResourceLayerExportCaps->taskManager           = new TaskManager();
	ResourceLayerExportCaps->virtualMemoryManager  = new VirtualMemoryManager();
	ResourceLayerExportCaps->initRamFileSystemManager = new InitRamFileSystemManager(platform.initramfs_size, platform.initramfs_address);

	return ResourceLayerExportCaps;
}

ResourceLayerCaps* ResourceLayerFactory::GetCaps() const
{
	return ResourceLayerExportCaps;
}
