#pragma once

struct ResourceLayerCaps;
class Scheduler;
class ProcessController;
class VirtualFileSystem;
class ElfMapper;
class CredentialManager;
class EventManager;
class SynchronizationManager;
class SignalManager;
class SocketManager;
class InterProcessCommunicationManager;

struct LogicLayerCaps
{
    Scheduler*              scheduler;
    ProcessController*      processController;
    VirtualFileSystem*      virtualFileSystem;
    ElfMapper*              elfMapper;
    CredentialManager*      credentialManager;
    EventManager*           eventManager;
    SynchronizationManager* synchronizationManager;
    SignalManager*          signalManager;
    SocketManager*          socketManager;
    InterProcessCommunicationManager* interProcessCommunicationManager;
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
