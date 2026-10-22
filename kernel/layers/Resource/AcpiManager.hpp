#pragma once

#include "layers/Resource/DeviceManager.hpp"
#include <acpi/runtime.h>

class AcpiManager
{
public:
    explicit AcpiManager(DeviceManager* registry);
    ~AcpiManager();
    AcpiManager(const AcpiManager&) = delete;
    AcpiManager& operator=(const AcpiManager&) = delete;

    // BSP startup, after construction and before PCI enumeration/scheduling.
    // Initialization is cached; registration may be retried independently.
    acpi_runtime_state_t Initialize();
    acpi_runtime_state_t GetState() const;
    size_t CountDevices() const;
    const acpi_device_t* GetDevice(size_t index) const;
    const acpi_device_t* FindPciDevice(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function) const;
    DeviceId GetDeviceId(acpi_device_id_t identity) const;
    DeviceStatus GetRegistrationStatus() const;

    // Firmware operations may block. Task context after scheduler activation;
    // BSP bootstrap context is also supported before activation. No IRQ callers.
    acpi_status_t RefreshStatus(acpi_device_id_t identity, uint32_t* flags, uint32_t* firmwareError);
    acpi_status_t GetResources(acpi_device_id_t identity, bool possible, acpi_resources_t* resources);
    static void ReleaseResources(acpi_resources_t* resources);
    // pin is zero-based (INTA=0). This resolves routing, never allocates an IRQ.
    acpi_status_t ResolvePciInterrupt(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint8_t pin, acpi_pci_route_t* route);
    acpi_status_t ConfigureInterruptLink(acpi_device_id_t link, uint32_t resourceIndex, uint32_t gsi);
    // Single consuming queue; lost is cumulative, deadline is monotonic ns.
    // A zero deadline polls. Events themselves never trigger shutdown/ejection.
    acpi_status_t NextEvent(acpi_event_t* event, uint64_t* lost, uint64_t deadline = 0);
    // Final firmware operation only. Caller first quiesces the system.
    acpi_status_t Reset();
    acpi_status_t PowerOff();

private:
    DeviceManager* Registry;
    DeviceId* RegistryIds;
    size_t RegistryCapacity;
    DeviceStatus RegistrationStatus;
};
