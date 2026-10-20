#include "VirtualMemory.hpp"
#include "DebugManager.hpp"
#include "stats/VirtualMemoryStats.hpp"
#include <layers/Resource/VirtualMemoryManager.hpp>
#include <layers/Resource/ProcessManager.hpp>

#if DEBUG

static arx_debug_view_t memoryViews[5];

void DebugVirtualMemoryPublish(DebugManager& manager, VirtualMemoryManager* memory, ProcessManager* processes)
{
    arx_vmm_debug.processes = &processes->DebugStats;
    arx_vmm_debug.resourceGroup = &memory->DebugStats.group;
    const char* names[] = {"space", "regions", "backing", "writeback", "translate"};

    for (size_t index = 0; index < 5; index++)
    {
        memoryViews[index] = {names[index], "arx_vmm_debug_stats_t", "arx.memory.virtual", 1,
                              &arx_vmm_debug, sizeof(arx_vmm_debug), 0, 0};
    }

    manager.Register("memory", "arx_vmm_debug_stats_t", &arx_vmm_debug,
                     sizeof(arx_vmm_debug), 1, &arx_vmm_debug.group, memoryViews, 5);
    manager.Register("vmm", "VirtualMemoryStats", &memory->DebugStats,
                     sizeof(memory->DebugStats), 1, &memory->DebugStats.group);
    manager.DeclareUnavailable("elf", "Logic layer not constructed");
}

#endif
