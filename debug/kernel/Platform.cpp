#include "Platform.hpp"
#include "DebugManager.hpp"

#include <platform.h>

#if DEBUG

static arx_debug_view_t cpuView;
static arx_debug_view_t pciView;
static arx_debug_view_t pmmViews[5];
static arx_debug_view_t heapViews[3];

void DebugPlatformPublish(DebugManager& manager)
{
    arx_platform_debug_stats_t& stats = platform.debugStats;

    const char* pmmNames[] = {"nodes", "zones", "orders", "blocks", "pfn"};
    const char* heapNames[] = {"caches", "slabs", "address"};

    for (size_t index = 0; index < 5; index++)
    {
        pmmViews[index] = {pmmNames[index], "arx_pmm_debug_stats_t", "arx.memory.pmm", 1,
                           &platform.debugPmm, sizeof(platform.debugPmm), 0, MAX_NUMA_NODES};
    }

    for (size_t index = 0; index < 3; index++)
    {
        heapViews[index] = {heapNames[index], "arx_heap_debug_stats_t", "arx.memory.heap", 1,
                            &platform.debugHeap, sizeof(platform.debugHeap), 0, MAX_NUMA_NODES};
    }

    manager.Register("pmm", "arx_pmm_debug_stats_t", &platform.debugPmm,
                    sizeof(platform.debugPmm), 1, nullptr, pmmViews, 5);
    manager.Register("heap", "arx_heap_debug_stats_t", &platform.debugHeap,
                    sizeof(platform.debugHeap), 1, nullptr, heapViews, 3);

    manager.DeclareUnavailable("platform", "platform publication pending");
    manager.DeclareUnavailable("cpus", "CPU publication pending");
    manager.DeclareUnavailable("pci", "PCI publication pending");

    cpuView = {"topology", "arx_cpu_debug_stats_t", "arx.platform.cpus", 1,
               &stats.cpus, sizeof(stats.cpus), 0, BOOT_SMP_MAX_CPUS};
    pciView = {"devices", "arx_pci_debug_stats_t", "arx.platform.pci", 1,
               &stats.pci, sizeof(stats.pci), 0, MAX_PCI_DEVICES};

    DebugManager::Result platformResult = manager.Publish(
        "platform", "arx_platform_debug_stats_t", &stats, sizeof(stats), 1);
    DebugManager::Result cpuResult = manager.Publish(
        "cpus", "arx_cpu_debug_stats_t", &stats.cpus, sizeof(stats.cpus), 1,
        nullptr, &cpuView, 1);
    DebugManager::Result pciResult = manager.Publish(
        "pci", "arx_pci_debug_stats_t", &stats.pci, sizeof(stats.pci), 1,
        nullptr, &pciView, 1);

    if (platformResult == DebugManager::Result::Ok
        && cpuResult == DebugManager::Result::Ok
        && pciResult == DebugManager::Result::Ok)
    {
        ARX_DEBUG_STORE(stats.providersPublished, 1);
        ARX_DEBUG_STORE(stats.stage, ARX_PLATFORM_PROVIDERS_PUBLISHED);
    }
}

#endif
