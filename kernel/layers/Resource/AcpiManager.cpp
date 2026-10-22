#include "layers/Resource/AcpiManager.hpp"

#include <klib/klib.h>

AcpiManager::AcpiManager(DeviceManager* registry)
{
    Registry = registry;
    RegistryIds = nullptr;
    RegistryCapacity = 0;
    RegistrationStatus = DeviceStatus::NotReady;
}

AcpiManager::~AcpiManager()
{
    // Quiescent teardown only. Platform identities remain valid even if a bound
    // driver prevents unregistering a record. Runtime is never unloaded here.
    for (size_t remaining = RegistryCapacity; Registry != nullptr && remaining != 0; remaining--)
    {
        if (RegistryIds[remaining - 1] != INVALID_DEVICE_ID)
        {
            Registry->UnregisterDevice(RegistryIds[remaining - 1]);
        }
    }

    kfree(RegistryIds);
}

acpi_runtime_state_t AcpiManager::Initialize()
{
    acpi_runtime_state_t state = platform_acpi_initialize();

    if (state.stage != ACPI_STAGE_READY || Registry == nullptr)
    {
        if (Registry != nullptr && state.status != ACPI_NOT_READY)
        {
            for (size_t i = 0; i < RegistryCapacity; i++)
            {
                if (RegistryIds[i] != INVALID_DEVICE_ID)
                {
                    Registry->SetDeviceState(RegistryIds[i], DeviceState::FAILED);
                }
            }
        }

        return state;
    }

    if (RegistryIds == nullptr && state.device_count != 0)
    {
        RegistryIds = static_cast<DeviceId*>(kzalloc(state.device_count * sizeof(DeviceId)));

        if (RegistryIds == nullptr)
        {
            RegistrationStatus = DeviceStatus::NoMemory;
            return state;
        }

        RegistryCapacity = state.device_count;
    }

    RegistrationStatus = DeviceStatus::Success;

    for (size_t i = 0; i < RegistryCapacity; i++)
    {
        if (RegistryIds[i] != INVALID_DEVICE_ID)
        {
            continue;
        }

        const acpi_device_t* identity = platform_acpi_device(i);
        Device device;
        device.bus = DeviceBus::ACPI;
        device.identity = identity;
        device.type = identity->pci_root ? DeviceType::CONTROLLER : DeviceType::UNKNOWN;

        if (identity->parent != 0)
        {
            device.parent_id = GetDeviceId(identity->parent);

            if (device.parent_id == INVALID_DEVICE_ID)
            {
                RegistrationStatus = DeviceStatus::NotReady;
                continue;
            }
        }

        device.state = identity->incomplete ? DeviceState::FAILED : DeviceState::DISCOVERED;
        DeviceStatus status = Registry->RegisterDevice(device, &RegistryIds[i]);

        if (status != DeviceStatus::Success)
        {
            RegistrationStatus = status;
        }
    }

    return state;
}

acpi_runtime_state_t AcpiManager::GetState() const
{
    return platform_acpi_state();
}

size_t AcpiManager::CountDevices() const
{
    return GetState().device_count;
}

const acpi_device_t* AcpiManager::GetDevice(size_t index) const
{
    return platform_acpi_device(index);
}

DeviceId AcpiManager::GetDeviceId(acpi_device_id_t identity) const
{
    if (identity == 0 || identity > RegistryCapacity)
    {
        return INVALID_DEVICE_ID;
    }

    return RegistryIds[identity - 1];
}

const acpi_device_t* AcpiManager::FindPciDevice(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function) const
{
    for (size_t i = 0; i < CountDevices(); i++)
    {
        const acpi_device_t* candidate = GetDevice(i);

        if (candidate != nullptr && candidate->has_pci_address && candidate->pci_segment == segment && candidate->pci_bus == bus &&
            candidate->pci_device == device && candidate->pci_function == function)
        {
            return candidate;
        }
    }

    return nullptr;
}

DeviceStatus AcpiManager::GetRegistrationStatus() const
{
    return RegistrationStatus;
}

acpi_status_t AcpiManager::RefreshStatus(acpi_device_id_t identity, uint32_t* flags, uint32_t* firmwareError)
{
    return platform_acpi_refresh(identity, flags, firmwareError);
}

acpi_status_t AcpiManager::GetResources(acpi_device_id_t identity, bool possible, acpi_resources_t* resources)
{
    return platform_acpi_resources(identity, possible, resources);
}

void AcpiManager::ReleaseResources(acpi_resources_t* resources)
{
    platform_acpi_free_resources(resources);
}

acpi_status_t AcpiManager::ResolvePciInterrupt(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint8_t pin, acpi_pci_route_t* route)
{
    return platform_acpi_route_pci(segment, bus, device, function, pin, route);
}

acpi_status_t AcpiManager::NextEvent(acpi_event_t* event, uint64_t* lost, uint64_t deadline)
{
    return platform_acpi_next_event(event, lost, deadline);
}

acpi_status_t AcpiManager::ConfigureInterruptLink(acpi_device_id_t link, uint32_t resourceIndex, uint32_t gsi)
{
    return platform_acpi_configure_link(link, resourceIndex, gsi);
}

acpi_status_t AcpiManager::Reset()
{
    return platform_acpi_reset();
}

acpi_status_t AcpiManager::PowerOff()
{
    return platform_acpi_power_off();
}
