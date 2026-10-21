#pragma once

#include <debug/kernel/stats/PciDebugStats.hpp>
#include <pci/pci.h>

enum class PciStatus
{
    NotEnumerated = PCI_NOT_ENUMERATED,
    Success       = PCI_SUCCESS,
    Unsupported   = PCI_UNSUPPORTED,
    NoRegions     = PCI_NO_REGIONS,
    Failed        = PCI_FAILED
};

class PciManager
{
public:
    ARX_DEBUG_ONLY(PciDebugStats DebugStats{};)
    ARX_DEBUG_ONLY(void PublishDebug(DebugManager& manager);)

    explicit PciManager(const platform_pci_ops_t* operations);
    PciManager(const PciManager&) = delete;
    PciManager& operator=(const PciManager&) = delete;

    // BSP startup only, before post-init tasks. Runs the backend once, caching
    // failures too. Queries never trigger discovery. Concurrent reads are safe
    // after startup publishes the completed inventory to other CPUs.
    PciStatus EnumerateDevices();
    PciStatus GetStatus() const;
    bool IsTruncated() const;
    size_t CountDevices() const;

    // Original platform records, borrowed for the kernel lifetime. Fields are
    // captured during discovery; BAR sizes and live config access are absent.
    const pci_device_t* GetDevice(size_t index) const;
    const pci_device_t* FindDevice(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function) const;

private:
    platform_pci_ops_t       Operations;
    platform_pci_inventory_t Inventory;
};
