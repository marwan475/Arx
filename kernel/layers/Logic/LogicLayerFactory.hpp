#pragma once

struct ResourceLayerCaps;
class Scheduler;
class VirtualFileSystem;

struct LogicLayerCaps
{
	Scheduler* scheduler;
	VirtualFileSystem* virtualFileSystem;
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
