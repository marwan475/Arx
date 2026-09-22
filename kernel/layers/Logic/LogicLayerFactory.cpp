#include "layers/Logic/LogicLayerFactory.hpp"

#include "layers/Logic/Scheduler.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

LogicLayerFactory::LogicLayerFactory()
{
	LogicLayerExportCaps = nullptr;
}

LogicLayerFactory::~LogicLayerFactory()
{
	if (LogicLayerExportCaps != nullptr)
	{
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

	LogicLayerExportCaps            = new LogicLayerCaps();
	LogicLayerExportCaps->scheduler = new Scheduler(resourceLayerCaps);

	return LogicLayerExportCaps;
}

LogicLayerCaps* LogicLayerFactory::GetCaps() const
{
	return LogicLayerExportCaps;
}
