#pragma once

#include "layers/Resource/ResourceFileSystem.hpp"

class PhysicalMemoryManager;
class ProcessManager;
class ProcessMemoryManager;
class TimeManager;
class VirtualMemoryManager;
class TaskManager;
class InitRamFileSystemManager;

struct ResourceLayerCaps
{
    PhysicalMemoryManager*       physicalMemoryManager;
    ProcessManager*              processManager;
    VirtualMemoryManager*        virtualMemoryManager;
    ProcessMemoryManager*        processMemoryManager;
    TimeManager*                 timeManager;
    TaskManager*                 taskManager;
    InitRamFileSystemManager*    initRamFileSystemManager;
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
    ResourceLayerCaps*  ResourceLayerExportCaps;
    ResourceFileSystem* ResourceFileSystemExport;
};
