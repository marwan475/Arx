#pragma once

#include <debug/kernel/DebugRecords.h>

#if DEBUG

#include <pci/pci.h>

class DebugManager;
class PciManager;

// pci-manager stats v1: the production manager owns this record. Dispatcher
// publishes it after Resource construction and invalidates all providers before
// destroying the factories. Temporary test managers do not self-register.
// status uses pci_enumeration_status_t (not arx_pci_outcome_t). inventoryAddress
// is the borrowed original PCI array, deviceCount is device functions, and
// truncated means discovery stopped at MAX_PCI_DEVICES. No addresses are MMIO
// roots and no callbacks are invoked by inspection.
// The snapshot describes the last completed EnumerateDevices call. activeWriters
// brackets each call, including cached results; a nonzero value means the call
// was interrupted and fields may not yet agree. Calls are BSP-startup-only.
// Counters start at construction, never reset, and wrap modulo 2^64. Queries do
// not change them. backendCalls counts callback invocations, not hardware scans:
// the platform callback can itself return its cached result.
struct PciDebugStats
{
    PciManager*       source;
    arx_debug_group_t group;
    uint64_t          backendAvailable;
    uint64_t          enumerateCalls;
    uint64_t          backendCalls;
    uint64_t          cachedCalls;
    uint64_t          rejectedResults;
    uint64_t          status;
    uint64_t          inventoryAddress;
    uint64_t          deviceCount;
    uint64_t          truncated;
};

class DebugPciEnumeration
{
public:
    DebugPciEnumeration(PciDebugStats& stats, const platform_pci_inventory_t& inventory)
        : Stats(stats), Inventory(inventory)
    {
        arx_debug_group_begin(&Stats.group);
        ARX_DEBUG_ADD(Stats.enumerateCalls, 1);
    }

    ~DebugPciEnumeration()
    {
        ARX_DEBUG_STORE(Stats.status, Inventory.status);
        ARX_DEBUG_STORE(Stats.inventoryAddress, reinterpret_cast<uintptr_t>(Inventory.devices));
        ARX_DEBUG_STORE(Stats.deviceCount, Inventory.count);
        ARX_DEBUG_STORE(Stats.truncated, Inventory.truncated);
        arx_debug_group_end(&Stats.group);
    }

    DebugPciEnumeration(const DebugPciEnumeration&) = delete;
    DebugPciEnumeration& operator=(const DebugPciEnumeration&) = delete;

private:
    PciDebugStats&                   Stats;
    const platform_pci_inventory_t& Inventory;
};

#endif
