#pragma once

#include "layers/Resource/DeviceManager.hpp"

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

    // Registry is borrowed and must outlive this manager. The controller identity
    // points to this manager, which must remain alive while snapshots are usable.
    PciManager(const platform_pci_ops_t* operations, DeviceManager* devices);
    // Quiescent teardown only: release drivers and external children first, and
    // stop all snapshot users. Unregisters owned records, never platform storage.
    ~PciManager();
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

    DeviceId GetControllerDeviceId() const;
    DeviceId GetDeviceId(size_t index) const;
    // Separate from hardware discovery. NotReady until a successful inventory
    // is registered. Explicit enumeration retries registration, never rescanning.
    DeviceStatus GetRegistrationStatus() const;

private:
    PciStatus DiscoverDevices();
    DeviceStatus RegisterController();
    void RegisterDevices();

    platform_pci_ops_t       Operations;
    platform_pci_inventory_t Inventory;
    DeviceManager*           Devices;
    DeviceId                 ControllerId;
    DeviceId                 DeviceIds[MAX_PCI_DEVICES];
    DeviceStatus             RegistrationStatus;
};
