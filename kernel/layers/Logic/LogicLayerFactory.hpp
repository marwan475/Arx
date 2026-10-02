#pragma once

struct ResourceLayerCaps;
class Scheduler;
class VirtualFileSystem;
class ElfMapper;

struct LogicLayerCaps
{
    Scheduler*         scheduler;
    VirtualFileSystem* virtualFileSystem;
    ElfMapper*         elfMapper;
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
