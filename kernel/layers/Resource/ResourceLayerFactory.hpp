#pragma once

class PhysicalMemoryManager;
class ProcessManager;
class VirtualMemoryManager;
class TaskManager;

struct ResourceLayerCaps
{
    PhysicalMemoryManager* physicalMemoryManager;
    ProcessManager*        processManager;
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
