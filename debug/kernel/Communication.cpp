#include "DebugManager.hpp"
#include "layers/Logic/InterProcessCommunicationManager.hpp"
#include "layers/Logic/SocketManager.hpp"

#if DEBUG

void InterProcessCommunicationManager::PublishDebug(DebugManager& manager, const ProcessDebugStats* processes)
{
    DebugStats.source = this;
    DebugStats.processes = processes;
    DebugStats.pipeOperations = &PipeOperations;
    DebugStats.memoryOperations = &MemoryOperations;

    static arx_debug_view_t views[2];
    const char* names[] = {"pipes", "memory-file"};

    for (size_t index = 0; index < 2; ++index)
    {
        views[index] = {names[index], "IpcDebugStats", "arx.ipc", 1,
                        &DebugStats, sizeof(DebugStats), 0, 0};
    }

    manager.Publish("ipc", "IpcDebugStats", &DebugStats, sizeof(DebugStats),
                    1, &DebugStats.group, views, 2);
}

void SocketManager::PublishDebug(DebugManager& manager, const ProcessDebugStats* processes)
{
    DebugStats.source = this;
    DebugStats.processes = processes;
    DebugStats.head = &Endpoints;
    DebugStats.count = &EndpointCount;
    DebugStats.operations = &Operations;
    DebugStats.capacity = 128;

    static arx_debug_view_t views[2];
    const char* names[] = {"endpoints", "messages"};

    for (size_t index = 0; index < 2; ++index)
    {
        views[index] = {names[index], "SocketDebugStats", "arx.sockets", 1,
                        &DebugStats, sizeof(DebugStats), 0, 128};
    }

    manager.Publish("sockets", "SocketDebugStats", &DebugStats, sizeof(DebugStats),
                    1, &DebugStats.group, views, 2);
}

#endif
