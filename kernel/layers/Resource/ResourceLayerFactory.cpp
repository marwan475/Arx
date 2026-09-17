#include "layers/Resource/ResourceLayerFactory.hpp"

#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

ResourceLayerFactory::ResourceLayerFactory()
{
	ResourceLayerExportCaps = nullptr;
}

ResourceLayerFactory::~ResourceLayerFactory()
{
	if (ResourceLayerExportCaps != nullptr)
	{
		delete ResourceLayerExportCaps->physicalMemoryManager;
		delete ResourceLayerExportCaps->virtualMemoryManager;
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
	ResourceLayerExportCaps->virtualMemoryManager  = new VirtualMemoryManager();

	return ResourceLayerExportCaps;
}

ResourceLayerCaps* ResourceLayerFactory::GetCaps() const
{
	return ResourceLayerExportCaps;
}
