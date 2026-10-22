#pragma once

#include "layers/Resource/ResourceFileSystem.hpp"

class DeviceManager;
class DmaManager;
class PciManager;
class PhysicalMemoryManager;
class ProcessManager;
class TimeManager;
class VirtualMemoryManager;
class TaskManager;
class InitRamFileSystemManager;

struct ResourceLayerCaps
{
    DeviceManager*               deviceManager;
    DmaManager*                  dmaManager;
    PciManager*                  pciManager;
    PhysicalMemoryManager*       physicalMemoryManager;
    ProcessManager*              processManager;
    VirtualMemoryManager*        virtualMemoryManager;
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
