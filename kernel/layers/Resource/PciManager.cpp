#include "layers/Resource/PciManager.hpp"

#include <debug/kernel/DebugManager.hpp>
#include <klib/klib.h>

static DeviceType ClassifyDevice(const pci_device_t& device)
{
    switch (device.class_code)
    {
        case 0x01:
            return DeviceType::STORAGE;
        case 0x02:
            return DeviceType::NETWORK;
        case 0x03:
            return DeviceType::DISPLAY;
        case 0x04:
            if (device.subclass == 0x01 || device.subclass == 0x03)
            {
                return DeviceType::AUDIO;
            }

            break;
        case 0x06:
            return DeviceType::CONTROLLER;
        case 0x07:
            if (device.subclass == 0x00)
            {
                return DeviceType::SERIAL;
            }

            break;
        case 0x09:
            return DeviceType::INPUT;
        case 0x0c:
            return DeviceType::CONTROLLER;
    }

    return DeviceType::UNKNOWN;
}

PciManager::PciManager(const platform_pci_ops_t* operations, DeviceManager* devices)
    : Operations{}, Inventory{ nullptr, 0, PCI_NOT_ENUMERATED, false },
      Devices(devices), ControllerId(INVALID_DEVICE_ID), DeviceIds{}, RegistrationStatus(DeviceStatus::NotReady)
{
    if (operations != nullptr)
    {
        Operations = *operations;
    }

    DeviceStatus status = RegisterController();

    if (status != DeviceStatus::Success)
    {
        RegistrationStatus = status;
    }

    ARX_DEBUG_RUN(
        DebugStats.source = this;
        DebugStats.backendAvailable = Operations.enumerate != nullptr;
        DebugStats.status = PCI_NOT_ENUMERATED;
    );
}

PciManager::~PciManager()
{
    if (Devices == nullptr)
    {
        return;
    }

    for (size_t index = 0; index < Inventory.count; index++)
    {
        DeviceStatus status = Devices->UnregisterDevice(DeviceIds[index]);

        if (status != DeviceStatus::Success && status != DeviceStatus::NotFound)
        {
            error_logger("Arx kernel: destroying PCI manager with live device users\n");
            panic();
        }
    }

    DeviceStatus status = Devices->UnregisterDevice(ControllerId);

    if (status != DeviceStatus::Success && status != DeviceStatus::NotFound)
    {
        error_logger("Arx kernel: destroying PCI manager with live controller users\n");
        panic();
    }
}

PciStatus PciManager::EnumerateDevices()
{
    ARX_DEBUG_ONLY(DebugPciEnumeration debugEnumeration(DebugStats, Inventory);)

    DeviceStatus controllerStatus = RegisterController();

    if (controllerStatus == DeviceStatus::Success)
    {
        controllerStatus = Devices->SetDeviceState(ControllerId, DeviceState::INITIALIZING);
    }

    PciStatus status = DiscoverDevices();

    if (controllerStatus != DeviceStatus::Success)
    {
        RegistrationStatus = controllerStatus;
        return status;
    }

    if (status != PciStatus::Success)
    {
        RegistrationStatus = DeviceStatus::NotReady;
        Devices->SetDeviceState(ControllerId, DeviceState::FAILED);
        return status;
    }

    RegisterDevices();

    return status;
}

PciStatus PciManager::DiscoverDevices()
{
    if (Inventory.status != PCI_NOT_ENUMERATED)
    {
        ARX_DEBUG_ADD(DebugStats.cachedCalls, 1);
        return GetStatus();
    }

    Inventory.status = PCI_UNSUPPORTED;

    if (Operations.enumerate == nullptr)
    {
        return GetStatus();
    }

    ARX_DEBUG_ADD(DebugStats.backendCalls, 1);
    platform_pci_inventory_t inventory = Operations.enumerate(Operations.context);

    switch (inventory.status)
    {
        case PCI_SUCCESS:
            if (inventory.count > MAX_PCI_DEVICES || (inventory.count != 0 && inventory.devices == nullptr))
            {
                ARX_DEBUG_ADD(DebugStats.rejectedResults, 1);
                Inventory.status = PCI_FAILED;
                return GetStatus();
            }

            Inventory = inventory;
            break;
        case PCI_UNSUPPORTED:
        case PCI_NO_REGIONS:
        case PCI_FAILED:
            Inventory.status = inventory.status;
            break;
        default:
            ARX_DEBUG_ADD(DebugStats.rejectedResults, 1);
            Inventory.status = PCI_FAILED;
            break;
    }

    return GetStatus();
}

DeviceStatus PciManager::RegisterController()
{
    if (Devices == nullptr)
    {
        return DeviceStatus::InvalidArgument;
    }

    Device controller;

    if (ControllerId != INVALID_DEVICE_ID && Devices->GetDevice(ControllerId, &controller) == DeviceStatus::Success)
    {
        return DeviceStatus::Success;
    }

    controller = {};
    controller.parent_id = CONTROLLER_DEVICE_ID;
    controller.type      = DeviceType::CONTROLLER;
    controller.identity  = this;
    ControllerId         = INVALID_DEVICE_ID;

    return Devices->RegisterDevice(controller, &ControllerId);
}

void PciManager::RegisterDevices()
{
    for (size_t index = 0; index < Inventory.count; index++)
    {
        Device device;

        if (DeviceIds[index] != INVALID_DEVICE_ID && Devices->GetDevice(DeviceIds[index], &device) == DeviceStatus::Success)
        {
            continue;
        }

        device = {};
        device.parent_id = ControllerId;
        device.bus       = DeviceBus::PCI;
        device.type      = ClassifyDevice(Inventory.devices[index]);
        device.identity  = &Inventory.devices[index];
        DeviceIds[index] = INVALID_DEVICE_ID;

        RegistrationStatus = Devices->RegisterDevice(device, &DeviceIds[index]);

        if (RegistrationStatus != DeviceStatus::Success)
        {
            Devices->SetDeviceState(ControllerId, DeviceState::FAILED);
            return;
        }
    }

    RegistrationStatus = Devices->SetDeviceState(ControllerId, DeviceState::INITIALIZED);
}

DeviceId PciManager::GetControllerDeviceId() const
{
    return ControllerId;
}

DeviceId PciManager::GetDeviceId(size_t index) const
{
    if (index >= Inventory.count)
    {
        return INVALID_DEVICE_ID;
    }

    return DeviceIds[index];
}

DeviceStatus PciManager::GetRegistrationStatus() const
{
    return RegistrationStatus;
}

PciStatus PciManager::GetStatus() const
{
    return static_cast<PciStatus>(Inventory.status);
}

bool PciManager::IsTruncated() const
{
    return Inventory.truncated;
}

size_t PciManager::CountDevices() const
{
    return Inventory.count;
}

const pci_device_t* PciManager::GetDevice(size_t index) const
{
    if (index >= Inventory.count)
    {
        return nullptr;
    }

    return &Inventory.devices[index];
}

const pci_device_t* PciManager::FindDevice(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function) const
{
    if (device >= 32 || function >= 8)
    {
        return nullptr;
    }

    for (size_t index = 0; index < Inventory.count; index++)
    {
        const pci_device_t* entry = &Inventory.devices[index];

        if (entry->segment == segment && entry->bus == bus && entry->device == device && entry->function == function)
        {
            return entry;
        }
    }

    return nullptr;
}

#if DEBUG

void PciManager::PublishDebug(DebugManager& manager)
{
    manager.Register("pci-manager", "PciDebugStats", &DebugStats, sizeof(DebugStats), 1, &DebugStats.group);
}

#endif
