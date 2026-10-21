#include "layers/Resource/PciManager.hpp"

PciManager::PciManager(const platform_pci_ops_t* operations)
    : Operations{}, Inventory{ nullptr, 0, PCI_NOT_ENUMERATED, false }
{
    if (operations != nullptr)
    {
        Operations = *operations;
    }
}

PciStatus PciManager::EnumerateDevices()
{
    if (Inventory.status != PCI_NOT_ENUMERATED)
    {
        return GetStatus();
    }

    Inventory.status = PCI_UNSUPPORTED;

    if (Operations.enumerate == nullptr)
    {
        return GetStatus();
    }

    platform_pci_inventory_t inventory = Operations.enumerate(Operations.context);

    switch (inventory.status)
    {
        case PCI_SUCCESS:
            if (inventory.count > MAX_PCI_DEVICES || (inventory.count != 0 && inventory.devices == nullptr))
            {
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
            Inventory.status = PCI_FAILED;
            break;
    }

    return GetStatus();
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
