#pragma once

struct ResourceLayerCaps;
class Scheduler;
class VirtualFileSystem;
class ElfMapper;
class CredentialManager;
class EventManager;

struct LogicLayerCaps
{
    Scheduler*         scheduler;
    VirtualFileSystem* virtualFileSystem;
    ElfMapper*         elfMapper;
    CredentialManager* credentialManager;
    EventManager*      eventManager;
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
