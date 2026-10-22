#include "layers/Resource/PciManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

#include <debug/kernel/DebugManager.hpp>
#include <platform.h>
#include <selftests/ManagerSuite.hpp>

struct PciMock
{
    platform_pci_inventory_t inventory {};
    size_t calls = 0;
    ARX_DEBUG_ONLY(const PciDebugStats* debugStats = nullptr;)
    ARX_DEBUG_ONLY(bool sawActiveEnumeration = false;)
};

static platform_pci_inventory_t MockEnumerate(void* context)
{
    PciMock* mock = static_cast<PciMock*>(context);
    mock->calls++;

    ARX_DEBUG_RUN(
        if (mock->debugStats != nullptr)
        {
            mock->sawActiveEnumeration = arx_debug_counter_read(&mock->debugStats->group.activeWriters) != 0;
        }
    );

    return mock->inventory;
}

void test_PciManager(ResourceLayerCaps* resources)
{
    PciManager*   manager = resources != nullptr ? resources->pciManager : nullptr;
    ManagerSuite suite("resource/PciManager", manager != nullptr && resources->deviceManager != nullptr);

    suite.Run("resource/PciManager/not_enumerated", [&](TestCase& t)
    {
        PciMock            mock;
        platform_pci_ops_t operations { &mock, MockEnumerate };
        DeviceManager      registry;
        PciManager         isolated(&operations, &registry);

        t.Check(isolated.GetStatus() == PciStatus::NotEnumerated, "construction does not enumerate");
        t.Equal(isolated.CountDevices(), 0, "no devices before explicit enumeration");
        t.Check(isolated.GetDevice(0) == nullptr, "indexed query is empty before enumeration");
        t.Check(isolated.FindDevice(0, 0, 0, 0) == nullptr, "address query is empty before enumeration");
        t.Check(!isolated.IsTruncated(), "initial inventory is not truncated");
        t.Equal(mock.calls, 0, "queries never invoke the backend");
        t.Check(manager->GetStatus() == PciStatus::NotEnumerated, "live enumeration waits until StartKernel returns");

        Device controller;
        t.Check(registry.GetDevice(isolated.GetControllerDeviceId(), &controller) == DeviceStatus::Success, "controller registered before discovery");
        t.Check(controller.bus == DeviceBus::PLATFORM && controller.type == DeviceType::CONTROLLER, "controller is a platform device");
        t.Equal(controller.parent_id, CONTROLLER_DEVICE_ID, "controller uses reserved root marker");
        t.Check(controller.identity == &isolated, "controller points to its PciManager");
        t.Check(controller.state == DeviceState::DISCOVERED, "controller begins discovered");
        t.Equal(registry.CountDevices(), 1, "construction registers only the controller");
        t.Check(isolated.GetRegistrationStatus() == DeviceStatus::NotReady, "function publication has not started");
        t.Equal(isolated.GetDeviceId(0), INVALID_DEVICE_ID, "no function ID before discovery");

        t.Check(resources->deviceManager->GetDevice(manager->GetControllerDeviceId(), &controller) == DeviceStatus::Success,
                "factory creates registry before live PCI controller");
        t.Check(controller.identity == manager, "live host controller points to live manager");

        ARX_DEBUG_RUN(
            t.Equal(isolated.DebugStats.enumerateCalls, 0, "queries do not change enumeration statistics");
            t.Equal(isolated.DebugStats.status, PCI_NOT_ENUMERATED, "debug state begins unstarted");
            t.Equal(isolated.DebugStats.backendAvailable, 1, "debug state records backend availability");
            t.Check(isolated.DebugStats.source == &isolated, "debug record belongs to its manager");

            DebugManager* registry = new DebugManager();

            if (t.Check(registry != nullptr, "isolated debug registry allocated outside the kernel stack"))
            {
                isolated.PublishDebug(*registry);
                t.Equal(registry->records[0].state, ARX_DEBUG_READY, "composition publishes ready provider");
                t.Check(registry->records[0].address == &isolated.DebugStats, "provider borrows manager statistics");
                t.Check(registry->records[0].group == &isolated.DebugStats.group, "provider exposes interrupted enumeration group");
                t.Check(registry->Withdraw("pci-manager", &isolated.DebugStats) == DebugManager::Result::Ok,
                        "provider withdraws before borrowed storage expires");

                delete registry;
            }
        );
    });

    suite.Run("resource/PciManager/discovery_identity_and_lookup", [&](TestCase& t)
    {
        static pci_device_t devices[3] {};
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
            DeviceManager      registry;
            PciManager         temporary(&operations, &registry);
            operations.enumerate = nullptr;
            ARX_DEBUG_RUN(mock.debugStats = &temporary.DebugStats;);

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

            ARX_DEBUG_RUN(
                t.Check(mock.sawActiveEnumeration, "backend runs inside marked debug operation");
                t.Equal(temporary.DebugStats.enumerateCalls, 2, "all explicit enumeration calls counted");
                t.Equal(temporary.DebugStats.backendCalls, 1, "backend invocation counted separately");
                t.Equal(temporary.DebugStats.cachedCalls, 1, "cached result counted separately");
                t.Equal(temporary.DebugStats.status, PCI_SUCCESS, "debug outcome remains cached success");
                t.Equal(temporary.DebugStats.inventoryAddress, reinterpret_cast<uintptr_t>(devices), "debug root preserves inventory address");
                t.Equal(temporary.DebugStats.deviceCount, 3, "debug count matches cached snapshot");
                t.Equal(temporary.DebugStats.group.activeWriters, 0, "enumeration marker always closes");
                t.Equal(temporary.DebugStats.group.completedUpdates, 2, "cached and fresh operations both finish");
            );
        }

        t.Equal(devices[2].function, 7, "destruction leaves borrowed records intact");
    });

    suite.Run("resource/PciManager/empty_and_failure_status", [&](TestCase& t)
    {
        DeviceManager registry;
        PciManager unavailable(nullptr, &registry);
        t.Check(unavailable.EnumerateDevices() == PciStatus::Unsupported, "missing backend reports unsupported");
        t.Check(unavailable.EnumerateDevices() == PciStatus::Unsupported, "missing backend result is cached");

        ARX_DEBUG_RUN(
            t.Equal(unavailable.DebugStats.backendAvailable, 0, "missing backend is visible");
            t.Equal(unavailable.DebugStats.backendCalls, 0, "missing backend never counts an invocation");
            t.Equal(unavailable.DebugStats.enumerateCalls, 2, "missing backend calls still counted");
            t.Equal(unavailable.DebugStats.cachedCalls, 1, "missing backend outcome is cached");
            t.Equal(unavailable.DebugStats.status, PCI_UNSUPPORTED, "unsupported debug outcome recorded");
            t.Equal(unavailable.DebugStats.group.activeWriters, 0, "missing backend closes debug group");
        );

        const pci_enumeration_status_t statuses[] = {
            PCI_SUCCESS, PCI_UNSUPPORTED, PCI_NO_REGIONS, PCI_FAILED
        };

        for (pci_enumeration_status_t status : statuses)
        {
            PciMock            mock { { nullptr, 0, status, false }, 0 };
            platform_pci_ops_t operations { &mock, MockEnumerate };
            DeviceManager      registry;
            PciManager         isolated(&operations, &registry);

            t.Check(isolated.EnumerateDevices() == static_cast<PciStatus>(status), "empty result preserves its reason");
            t.Equal(isolated.CountDevices(), 0, "empty or failed result exposes no records");
            t.Check(isolated.GetDevice(0) == nullptr, "empty or failed result has no indexed record");

            mock.inventory.status = PCI_SUCCESS;
            t.Check(isolated.EnumerateDevices() == static_cast<PciStatus>(status), "failure and empty results are cached");
            t.Equal(mock.calls, 1, "each outcome invokes the backend only once");

            Device controller;
            t.Check(registry.GetDevice(isolated.GetControllerDeviceId(), &controller) == DeviceStatus::Success, "empty and failed discovery retain controller");
            DeviceState expected = status == PCI_SUCCESS ? DeviceState::INITIALIZED : DeviceState::FAILED;
            t.Check(controller.state == expected, "controller state reflects discovery outcome");
            t.Equal(registry.CountDevices(), 1, "empty and failed discovery create no function records");

            ARX_DEBUG_RUN(
                t.Equal(isolated.DebugStats.status, status, "empty and failure debug outcomes agree");
                t.Equal(isolated.DebugStats.backendCalls, 1, "empty and failure backend calls counted");
                t.Equal(isolated.DebugStats.cachedCalls, 1, "empty and failure cache hits counted");
                t.Equal(isolated.DebugStats.group.activeWriters, 0, "every outcome closes debug group");
            );
        }
    });

    suite.Run("resource/PciManager/truncated_inventory", [&](TestCase& t)
    {
        // Static storage avoids putting the fixed inventory on the kernel stack.
        static pci_device_t devices[MAX_PCI_DEVICES] {};
        PciMock            mock { { devices, MAX_PCI_DEVICES, PCI_SUCCESS, true }, 0 };
        platform_pci_ops_t operations { &mock, MockEnumerate };
        DeviceManager      registry;
        PciManager         isolated(&operations, &registry);

        t.Check(isolated.EnumerateDevices() == PciStatus::Success, "partial discovery remains usable");
        t.Check(isolated.IsTruncated(), "capacity exhaustion exposed independently of success");
        t.Equal(isolated.CountDevices(), MAX_PCI_DEVICES, "full recorded inventory retained");
        t.Check(isolated.GetDevice(MAX_PCI_DEVICES - 1) == &devices[MAX_PCI_DEVICES - 1], "last recorded device is accessible");
        t.Check(isolated.GetDevice(MAX_PCI_DEVICES) == nullptr, "no traversal beyond capacity");

        ARX_DEBUG_RUN(
            t.Equal(isolated.DebugStats.truncated, 1, "debug snapshot retains truncation");
            t.Equal(isolated.DebugStats.deviceCount, MAX_PCI_DEVICES, "debug snapshot retains full recorded count");
        );
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
            DeviceManager      registry;
            PciManager         isolated(&operations, &registry);

            t.Check(isolated.EnumerateDevices() == PciStatus::Failed, "malformed backend result rejected");
            t.Equal(isolated.CountDevices(), 0, "invalid inventory is never exposed");
            t.Check(isolated.GetDevice(0) == nullptr, "invalid storage cannot be traversed");
            t.Check(isolated.EnumerateDevices() == PciStatus::Failed, "malformed result remains failed");
            t.Equal(mock.calls, 1, "malformed result cannot trigger a retry");

            ARX_DEBUG_RUN(
                t.Equal(isolated.DebugStats.rejectedResults, 1, "malformed result counted exactly once");
                t.Equal(isolated.DebugStats.status, PCI_FAILED, "malformed result records failure");
                t.Equal(isolated.DebugStats.inventoryAddress, 0, "malformed storage not published as inventory");
                t.Equal(isolated.DebugStats.deviceCount, 0, "malformed count not published as inventory");
                t.Equal(isolated.DebugStats.group.activeWriters, 0, "rejection closes debug group");
            );
        }
    });

    suite.Run("resource/PciManager/device_types", [&](TestCase& t)
    {
        struct Classification
        {
            uint8_t    classCode;
            uint8_t    subclass;
            DeviceType type;
        };

        const Classification classes[] = {
            { 0x01, 0x06, DeviceType::STORAGE },
            { 0x02, 0x00, DeviceType::NETWORK },
            { 0x03, 0x00, DeviceType::DISPLAY },
            { 0x04, 0x01, DeviceType::AUDIO },
            { 0x04, 0x03, DeviceType::AUDIO },
            { 0x04, 0x00, DeviceType::UNKNOWN },
            { 0x06, 0x04, DeviceType::CONTROLLER },
            { 0x07, 0x00, DeviceType::SERIAL },
            { 0x07, 0x01, DeviceType::UNKNOWN },
            { 0x09, 0x02, DeviceType::INPUT },
            { 0x0c, 0x03, DeviceType::CONTROLLER },
            { 0xff, 0x00, DeviceType::UNKNOWN }
        };

        constexpr size_t count = sizeof(classes) / sizeof(classes[0]);
        static pci_device_t devices[count] {};

        for (size_t index = 0; index < count; index++)
        {
            devices[index].class_code = classes[index].classCode;
            devices[index].subclass = classes[index].subclass;
        }

        DeviceManager registry;
        PciMock mock { { devices, count, PCI_SUCCESS, false }, 0 };
        platform_pci_ops_t operations { &mock, MockEnumerate };
        PciManager isolated(&operations, &registry);

        t.Check(isolated.EnumerateDevices() == PciStatus::Success, "typed inventory discovered");
        t.Check(isolated.GetRegistrationStatus() == DeviceStatus::Success, "typed inventory registered");

        for (size_t index = 0; index < count; index++)
        {
            Device device;
            t.Check(registry.GetDevice(isolated.GetDeviceId(index), &device) == DeviceStatus::Success, "function has generic representation");
            t.Check(device.type == classes[index].type, "functional type derived from class and subclass");
            t.Check(device.identity == &devices[index], "classification preserves original identity");
            t.Check(device.bus == DeviceBus::PCI && device.operations == nullptr, "function uses PCI bus without invented operations");
            t.Equal(device.parent_id, isolated.GetControllerDeviceId(), "function belongs to synthetic host controller");
            t.Check(device.state == DeviceState::DISCOVERED, "discovery does not initialize a driver");
        }

        t.Equal(isolated.GetDeviceId(count), INVALID_DEVICE_ID, "out-of-range function ID rejected");
    });

    suite.Run("resource/PciManager/registry_teardown", [&](TestCase& t)
    {
        static pci_device_t device {};
        DeviceManager registry;
        PciMock mock { { &device, 1, PCI_SUCCESS, false }, 0 };
        platform_pci_ops_t operations { &mock, MockEnumerate };
        DeviceId controllerId = INVALID_DEVICE_ID;
        DeviceId functionId = INVALID_DEVICE_ID;

        {
            PciManager isolated(&operations, &registry);
            t.Check(isolated.EnumerateDevices() == PciStatus::Success, "fixture discovery succeeds");
            controllerId = isolated.GetControllerDeviceId();
            functionId = isolated.GetDeviceId(0);
            t.Equal(registry.CountDevices(), 2, "controller and function live while manager is alive");
        }

        Device snapshot;
        t.Check(registry.GetDevice(controllerId, &snapshot) == DeviceStatus::NotFound, "controller removed before its manager identity expires");
        t.Check(registry.GetDevice(functionId, &snapshot) == DeviceStatus::NotFound, "owned function registration removed during quiescent teardown");
        t.Equal(registry.CountDevices(), 0, "teardown leaves no stale manager identities");
        t.Equal(mock.calls, 1, "teardown never invokes hardware discovery");
    });

    suite.Run("resource/PciManager/registration_retry", [&](TestCase& t)
    {
        static pci_device_t devices[3] {};
        DeviceManager registry;
        PciMock mock { { devices, 3, PCI_SUCCESS, false }, 0 };
        platform_pci_ops_t operations { &mock, MockEnumerate };
        PciManager isolated(&operations, &registry);

        // A conflicting identity forces deterministic partial publication without
        // replacing the kernel allocator or adding production-only test hooks.
        Device conflict;
        conflict.bus = DeviceBus::PCI;
        conflict.identity = &devices[1];
        DeviceId conflictId = INVALID_DEVICE_ID;
        t.Check(registry.RegisterDevice(conflict, &conflictId) == DeviceStatus::Success, "conflicting record installed");
        t.Check(isolated.EnumerateDevices() == PciStatus::Success, "registry failure does not change discovery success");
        t.Check(isolated.GetRegistrationStatus() == DeviceStatus::AlreadyExists, "publication failure is separately queryable");
        t.Equal(isolated.CountDevices(), 3, "hardware inventory remains complete");

        Device controller;
        t.Check(registry.GetDevice(isolated.GetControllerDeviceId(), &controller) == DeviceStatus::Success && controller.state == DeviceState::FAILED,
                "controller reflects failed publication");

        DeviceId first = isolated.GetDeviceId(0);
        t.Check(first != INVALID_DEVICE_ID, "earlier successful registration retained");
        t.Equal(isolated.GetDeviceId(1), INVALID_DEVICE_ID, "conflicting function not published");
        t.Equal(isolated.GetDeviceId(2), INVALID_DEVICE_ID, "publication stops at failure");
        t.Check(registry.SetDeviceState(first, DeviceState::INITIALIZED) == DeviceStatus::Success, "retained device state advanced");
        t.Check(registry.AssociateDriver(first, 77) == DeviceStatus::Success, "retained device bound");
        t.Check(registry.UnregisterDevice(conflictId) == DeviceStatus::Success, "publication obstruction removed");

        t.Check(isolated.EnumerateDevices() == PciStatus::Success, "registration retries against cached discovery");
        t.Check(isolated.GetRegistrationStatus() == DeviceStatus::Success, "missing registrations now complete");
        t.Equal(mock.calls, 1, "retry never rescans PCI");
        t.Equal(registry.CountDevices(), 4, "one controller and three unique functions registered");
        t.Equal(isolated.GetDeviceId(0), first, "retry preserves existing ID");

        Device retained;
        t.Check(registry.GetDevice(first, &retained) == DeviceStatus::Success && retained.driver_id == 77 && retained.state == DeviceState::INITIALIZED,
                "retry preserves device state and driver binding");
        t.Check(registry.GetDevice(isolated.GetControllerDeviceId(), &controller) == DeviceStatus::Success && controller.state == DeviceState::INITIALIZED,
                "controller recovers after successful registration");

        DeviceId removed = isolated.GetDeviceId(2);
        t.Check(registry.UnregisterDevice(removed) == DeviceStatus::Success, "unbound function explicitly removed");
        t.Check(isolated.EnumerateDevices() == PciStatus::Success, "missing function can be republished without discovery");
        t.Check(isolated.GetDeviceId(2) > removed, "republished function receives a new ID");
        t.Equal(registry.CountDevices(), 4, "repair does not duplicate remaining functions");
        t.Equal(mock.calls, 1, "repair also reuses cached inventory");
        t.Check(registry.DisassociateDriver(first) == DeviceStatus::Success, "fixture releases driver association");
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
    DeviceManager* registry = resources->deviceManager;

    if (!t.Check(registry != nullptr, "central device registry exists before PCI discovery"))
    {
        return;
    }

    Device controller;
    t.Check(registry->GetDevice(manager->GetControllerDeviceId(), &controller) == DeviceStatus::Success, "live PCI host controller registered");
    t.Check(controller.identity == manager, "host controller identity points to PciManager");
    t.Equal(controller.parent_id, CONTROLLER_DEVICE_ID, "host controller has controller parent marker");
    DeviceState controllerState = manager->GetStatus() == PciStatus::Success ? DeviceState::INITIALIZED : DeviceState::FAILED;
    t.Check(controller.state == controllerState, "live controller reflects discovery and registration outcome");
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

        Device generic;
        t.Check(registry->GetDevice(manager->GetDeviceId(index), &generic) == DeviceStatus::Success, "production function is in central registry");
        t.Check(generic.identity == device && generic.bus == DeviceBus::PCI, "generic record borrows production PCI identity");
        t.Equal(generic.parent_id, controller.id, "production function references live controller");
    }

    if (manager->GetStatus() == PciStatus::Success)
    {
        t.Check(manager->GetRegistrationStatus() == DeviceStatus::Success, "production registry publication succeeded");
    }

    platform_pci_inventory_t repeated = platform_pci_enumerate(nullptr);
    t.Check(repeated.devices == platform.pci_devices, "platform repeated call retains storage");
    t.Check(manager->EnumerateDevices() == static_cast<PciStatus>(repeated.status), "manager repeated call retains status");
}
