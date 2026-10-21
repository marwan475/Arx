#include "layers/Resource/PciManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

#include <platform.h>
#include <selftests/ManagerSuite.hpp>

struct PciMock
{
    platform_pci_inventory_t inventory {};
    size_t calls = 0;
};

static platform_pci_inventory_t MockEnumerate(void* context)
{
    PciMock* mock = static_cast<PciMock*>(context);
    mock->calls++;

    return mock->inventory;
}

void test_PciManager(ResourceLayerCaps* resources)
{
    PciManager*   manager = resources != nullptr ? resources->pciManager : nullptr;
    ManagerSuite suite("resource/PciManager", manager != nullptr);

    suite.Run("resource/PciManager/not_enumerated", [&](TestCase& t)
    {
        PciMock            mock;
        platform_pci_ops_t operations { &mock, MockEnumerate };
        PciManager         isolated(&operations);

        t.Check(isolated.GetStatus() == PciStatus::NotEnumerated, "construction does not enumerate");
        t.Equal(isolated.CountDevices(), 0, "no devices before explicit enumeration");
        t.Check(isolated.GetDevice(0) == nullptr, "indexed query is empty before enumeration");
        t.Check(isolated.FindDevice(0, 0, 0, 0) == nullptr, "address query is empty before enumeration");
        t.Check(!isolated.IsTruncated(), "initial inventory is not truncated");
        t.Equal(mock.calls, 0, "queries never invoke the backend");
        t.Check(manager->GetStatus() == PciStatus::NotEnumerated, "live enumeration waits until StartKernel returns");
    });

    suite.Run("resource/PciManager/discovery_identity_and_lookup", [&](TestCase& t)
    {
        pci_device_t devices[3] {};
        devices[0].bus      = 4;
        devices[0].device   = 2;
        devices[1].segment  = 1;
        devices[1].bus      = 4;
        devices[1].device   = 2;
        devices[2].segment  = 1;
        devices[2].bus      = 4;
        devices[2].device   = 2;
        devices[2].function = 7;

        PciMock mock { { devices, 3, PCI_SUCCESS, false }, 0 };

        {
            platform_pci_ops_t operations { &mock, MockEnumerate };
            PciManager        temporary(&operations);
            operations.enumerate = nullptr;

            t.Check(temporary.EnumerateDevices() == PciStatus::Success, "discovery succeeds");
            t.Equal(temporary.CountDevices(), 3, "all returned records exposed");
            t.Check(temporary.GetDevice(0) == &devices[0], "original first record identity preserved for DMA");
            t.Check(temporary.GetDevice(2) == &devices[2], "original last record identity preserved");
            t.Check(temporary.FindDevice(0, 4, 2, 0) == &devices[0], "lookup matches segment zero");
            t.Check(temporary.FindDevice(1, 4, 2, 0) == &devices[1], "segment is part of identity");
            t.Check(temporary.FindDevice(1, 4, 2, 7) == &devices[2], "function is part of identity");
            t.Check(temporary.FindDevice(1, 5, 2, 0) == nullptr, "different bus does not match");
            t.Check(temporary.FindDevice(1, 4, 3, 0) == nullptr, "different device does not match");
            t.Check(temporary.FindDevice(1, 4, 32, 0) == nullptr, "invalid device rejected");
            t.Check(temporary.FindDevice(1, 4, 2, 8) == nullptr, "invalid function rejected");
            t.Check(temporary.GetDevice(3) == nullptr, "index at count rejected");
            t.Check(temporary.GetDevice(SIZE_MAX) == nullptr, "maximum index rejected");

            mock.inventory = { nullptr, 0, PCI_FAILED, false };
            t.Check(temporary.EnumerateDevices() == PciStatus::Success, "repeated enumeration returns original success");
            t.Check(temporary.GetDevice(0) == &devices[0], "repeated call retains original address");
            t.Equal(mock.calls, 1, "successful backend invoked only once");
        }

        t.Equal(devices[2].function, 7, "destruction leaves borrowed records intact");
    });

    suite.Run("resource/PciManager/empty_and_failure_status", [&](TestCase& t)
    {
        PciManager unavailable(nullptr);
        t.Check(unavailable.EnumerateDevices() == PciStatus::Unsupported, "missing backend reports unsupported");
        t.Check(unavailable.EnumerateDevices() == PciStatus::Unsupported, "missing backend result is cached");

        const pci_enumeration_status_t statuses[] = {
            PCI_SUCCESS, PCI_UNSUPPORTED, PCI_NO_REGIONS, PCI_FAILED
        };

        for (pci_enumeration_status_t status : statuses)
        {
            PciMock            mock { { nullptr, 0, status, false }, 0 };
            platform_pci_ops_t operations { &mock, MockEnumerate };
            PciManager         isolated(&operations);

            t.Check(isolated.EnumerateDevices() == static_cast<PciStatus>(status), "empty result preserves its reason");
            t.Equal(isolated.CountDevices(), 0, "empty or failed result exposes no records");
            t.Check(isolated.GetDevice(0) == nullptr, "empty or failed result has no indexed record");

            mock.inventory.status = PCI_SUCCESS;
            t.Check(isolated.EnumerateDevices() == static_cast<PciStatus>(status), "failure and empty results are cached");
            t.Equal(mock.calls, 1, "each outcome invokes the backend only once");
        }
    });

    suite.Run("resource/PciManager/truncated_inventory", [&](TestCase& t)
    {
        // Static storage avoids putting the fixed inventory on the kernel stack.
        static pci_device_t devices[MAX_PCI_DEVICES] {};
        PciMock            mock { { devices, MAX_PCI_DEVICES, PCI_SUCCESS, true }, 0 };
        platform_pci_ops_t operations { &mock, MockEnumerate };
        PciManager         isolated(&operations);

        t.Check(isolated.EnumerateDevices() == PciStatus::Success, "partial discovery remains usable");
        t.Check(isolated.IsTruncated(), "capacity exhaustion exposed independently of success");
        t.Equal(isolated.CountDevices(), MAX_PCI_DEVICES, "full recorded inventory retained");
        t.Check(isolated.GetDevice(MAX_PCI_DEVICES - 1) == &devices[MAX_PCI_DEVICES - 1], "last recorded device is accessible");
        t.Check(isolated.GetDevice(MAX_PCI_DEVICES) == nullptr, "no traversal beyond capacity");
    });

    suite.Run("resource/PciManager/invalid_backend_result", [&](TestCase& t)
    {
        pci_device_t device {};
        const platform_pci_inventory_t invalid[] = {
            { nullptr, 1, PCI_SUCCESS, false },
            { &device, MAX_PCI_DEVICES + 1, PCI_SUCCESS, false },
            { &device, 1, PCI_NOT_ENUMERATED, false }
        };

        for (const platform_pci_inventory_t& inventory : invalid)
        {
            PciMock            mock { inventory, 0 };
            platform_pci_ops_t operations { &mock, MockEnumerate };
            PciManager         isolated(&operations);

            t.Check(isolated.EnumerateDevices() == PciStatus::Failed, "malformed backend result rejected");
            t.Equal(isolated.CountDevices(), 0, "invalid inventory is never exposed");
            t.Check(isolated.GetDevice(0) == nullptr, "invalid storage cannot be traversed");
            t.Check(isolated.EnumerateDevices() == PciStatus::Failed, "malformed result remains failed");
            t.Equal(mock.calls, 1, "malformed result cannot trigger a retry");
        }
    });
}

void test_PciManagerPlatform(ResourceLayerCaps* resources)
{
    TestCase t("resource/PciManager/platform_inventory");

    if (!t.Check(resources != nullptr && resources->pciManager != nullptr, "PCI capability exists after StartKernel"))
    {
        return;
    }

    PciManager* manager = resources->pciManager;
    t.Check(manager->GetStatus() != PciStatus::NotEnumerated, "kmain explicitly attempted enumeration");
    t.Check(manager->GetStatus() == static_cast<PciStatus>(platform.pci_status), "manager and platform status agree");
    t.Equal(manager->CountDevices(), platform.pci_device_count, "manager and DMA share inventory count");
    t.Check(manager->IsTruncated() == platform.pci_truncated, "truncation is available without debug statistics");

    for (size_t index = 0; index < manager->CountDevices(); index++)
    {
        const pci_device_t* device = manager->GetDevice(index);
        t.Check(device == &platform.pci_devices[index], "manager preserves production DMA pointer identity");
        t.Check(manager->FindDevice(device->segment, device->bus, device->device, device->function) != nullptr,
                "recorded address can be found");
    }

    platform_pci_inventory_t repeated = platform_pci_enumerate(nullptr);
    t.Check(repeated.devices == platform.pci_devices, "platform repeated call retains storage");
    t.Check(manager->EnumerateDevices() == static_cast<PciStatus>(repeated.status), "manager repeated call retains status");
}
