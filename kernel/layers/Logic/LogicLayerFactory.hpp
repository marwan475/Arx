#pragma once

struct ResourceLayerCaps;
class Scheduler;

struct LogicLayerCaps
{
	Scheduler* scheduler;
};

class LogicLayerFactory
{
public:
	LogicLayerFactory();
	~LogicLayerFactory();
	LogicLayerCaps* Create(ResourceLayerCaps* resourceLayerCaps);

	LogicLayerCaps* GetCaps() const;

private:
	LogicLayerCaps* LogicLayerExportCaps;
};
