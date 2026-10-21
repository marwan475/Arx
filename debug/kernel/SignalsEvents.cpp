#include "DebugManager.hpp"
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/SignalManager.hpp"
#include "layers/Logic/EventState.hpp"

#if DEBUG

SignalDebugStats SignalManager::DebugStats{};

void SignalManager::PublishDebug(DebugManager& manager, const ProcessDebugStats* processes)
{
    DebugStats.source = this;
    DebugStats.processes = processes;

    static arx_debug_view_t views[2];
    const char* names[] = {"pending", "dispositions"};

    for (size_t index = 0; index < 2; ++index)
    {
        views[index] = {names[index], "SignalDebugStats", "arx.signals", 1,
                        &DebugStats, sizeof(DebugStats), 0, 0};
    }

    manager.Publish("signals", "SignalDebugStats", &DebugStats, sizeof(DebugStats),
                    1, &DebugStats.group, views, 2);
}

void EventManager::PublishDebug(DebugManager& manager, const ProcessDebugStats* processes)
{
    DebugStats.source = this;
    DebugStats.processes = processes;
    DebugStats.signals = &SignalManager::DebugStats;
    DebugStats.counterOperations = &CounterOperations;
    DebugStats.epollOperations = &EpollOperations;
    DebugStats.inotifyOperations = &InotifyOperations;
    DebugStats.signalOperations = &SignalOperations;
    DebugStats.watchClosing = reinterpret_cast<const void*>(WatchClosing);
    DebugStats.epollClosing = reinterpret_cast<const void*>(EpollClosing);
    DebugStats.readinessLock = &EpollLock;

    static arx_debug_view_t views[3];
    const char* names[] = {"epoll", "notifications", "counter"};

    for (size_t index = 0; index < 3; ++index)
    {
        views[index] = {names[index], "EventDebugStats", "arx.events", 1,
                        &DebugStats, sizeof(DebugStats), 0, 0};
    }

    manager.Publish("events", "EventDebugStats", &DebugStats, sizeof(DebugStats),
                    1, &DebugStats.group, views, 3);
}

#endif
