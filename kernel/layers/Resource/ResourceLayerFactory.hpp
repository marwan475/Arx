#pragma once

class PhysicalMemoryManager;
class VirtualMemoryManager;

struct ResourceLayerCaps
{
    PhysicalMemoryManager* physicalMemoryManager;
    VirtualMemoryManager*  virtualMemoryManager;
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
