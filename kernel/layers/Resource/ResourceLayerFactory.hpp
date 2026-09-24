#pragma once

#include "layers/Resource/ResourceFileSystem.hpp"

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
    ResourceLayerFileSystemCaps* fileSystemCaps;
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
    ResourceFileSystem* ResourceFileSystemExport;
};
