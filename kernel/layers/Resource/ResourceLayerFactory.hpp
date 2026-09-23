#pragma once

class PhysicalMemoryManager;
class ProcessManager;
class VirtualMemoryManager;
class TaskManager;
class InitRamFileSystemManager;

struct ResourceLayerCaps
{
    PhysicalMemoryManager* physicalMemoryManager;
    ProcessManager*        processManager;
    VirtualMemoryManager*  virtualMemoryManager;
    TaskManager*           taskManager;
    InitRamFileSystemManager* initRamFileSystemManager;
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
