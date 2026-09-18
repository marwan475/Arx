#pragma once

class PhysicalMemoryManager;
class VirtualMemoryManager;
class TaskManager;

struct ResourceLayerCaps
{
    PhysicalMemoryManager* physicalMemoryManager;
    VirtualMemoryManager*  virtualMemoryManager;
    TaskManager*           taskManager;
};

class ResourceLayerFactory
{
public:
    ResourceLayerFactory();
    ~ResourceLayerFactory();
    ResourceLayerCaps* Create();

    ResourceLayerCaps* GetCaps() const;

private:
    ResourceLayerCaps* ResourceLayerExportCaps;
};
