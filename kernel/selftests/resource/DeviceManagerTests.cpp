#include "layers/Resource/DeviceManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

#include <selftests/ManagerSuite.hpp>

static const uint64_t DeviceTestIdentity = 0x1234;
static const uint64_t DeviceTestOperations = 0x5678;

void test_DeviceManager(ResourceLayerCaps* resources)
{
    ManagerSuite suite("resource/DeviceManager", resources != nullptr && resources->deviceManager != nullptr);

    suite.Run("resource/DeviceManager/registration_and_snapshots", [&](TestCase& t)
    {
        DeviceManager registry;
        Device        device;
        DeviceId      id = INVALID_DEVICE_ID;

        device.identity   = &DeviceTestIdentity;
        device.operations = &DeviceTestOperations;
        device.bus        = DeviceBus::VIRTUAL;
        device.type       = DeviceType::STORAGE;

        if (!t.Check(registry.RegisterDevice(device, &id) == DeviceStatus::Success, "device registration succeeds"))
        {
            return;
        }

        t.Check(id != INVALID_DEVICE_ID && id != CONTROLLER_DEVICE_ID, "assigned ID is not reserved");
        t.Equal(registry.CountDevices(), 1, "registration owns one record");
        t.Equal(device.id, INVALID_DEVICE_ID, "registration does not modify input");

        Device snapshot;
        t.Check(registry.GetDevice(id, &snapshot) == DeviceStatus::Success, "lookup returns a value snapshot");
        t.Check(snapshot.identity == &DeviceTestIdentity && snapshot.operations == &DeviceTestOperations, "opaque pointers preserved exactly");
        t.Check(snapshot.type == DeviceType::STORAGE && snapshot.bus == DeviceBus::VIRTUAL, "bus and functional type preserved");
        t.Check(snapshot.state == DeviceState::DISCOVERED && snapshot.driver_id == INVALID_DRIVER_ID, "new device is discovered and unbound");

        DeviceId duplicate = INVALID_DEVICE_ID;
        t.Check(registry.RegisterDevice(device, &duplicate) == DeviceStatus::AlreadyExists, "duplicate bus and identity rejected");
        t.Equal(duplicate, INVALID_DEVICE_ID, "failed registration preserves output");

        device.bus = DeviceBus::USB;
        t.Check(registry.RegisterDevice(device, &duplicate) == DeviceStatus::Success, "same identity on another bus is distinct");
        t.Check(registry.UnregisterDevice(duplicate) == DeviceStatus::Success, "second bus record removed");
        t.Check(registry.UnregisterDevice(id) == DeviceStatus::Success, "original removed");
        t.Check(registry.GetDevice(id, &device) == DeviceStatus::NotFound, "removed ID cannot be looked up");
        t.Equal(snapshot.id, id, "copied snapshot survives removal");
        t.Equal(*static_cast<const uint64_t*>(snapshot.identity), DeviceTestIdentity, "identity remains valid after removal");
        t.Equal(*static_cast<const uint64_t*>(snapshot.operations), DeviceTestOperations, "operations remain valid after removal");
        t.Check(registry.UnregisterDevice(id) == DeviceStatus::NotFound, "double removal rejected");

        device = {};
        DeviceId next = INVALID_DEVICE_ID;
        t.Check(registry.RegisterDevice(device, &next) == DeviceStatus::Success, "new synthetic device registered");
        t.Check(next > id && next > duplicate, "removed identifiers are never reused");
    });

    suite.Run("resource/DeviceManager/parents_and_removal", [&](TestCase& t)
    {
        DeviceManager registry;
        Device        controller;
        DeviceId      parent = INVALID_DEVICE_ID;
        DeviceId      childId = INVALID_DEVICE_ID;

        controller.type      = DeviceType::CONTROLLER;
        controller.parent_id = CONTROLLER_DEVICE_ID;

        if (!t.Check(registry.RegisterDevice(controller, &parent) == DeviceStatus::Success, "controller marker needs no parent record"))
        {
            return;
        }

        Device child;
        child.parent_id = parent;
        child.bus = DeviceBus::PCI;

        t.Check(registry.RegisterDevice(child, &childId) == DeviceStatus::Success, "child attaches to existing parent");
        t.Check(registry.UnregisterDevice(parent) == DeviceStatus::Busy, "parent cannot disappear beneath its child");
        t.Check(registry.GetDevice(CONTROLLER_DEVICE_ID, &controller) == DeviceStatus::NotFound, "controller marker is not a device");
        t.Check(registry.UnregisterDevice(childId) == DeviceStatus::Success, "leaf can be removed");
        t.Check(registry.UnregisterDevice(parent) == DeviceStatus::Success, "childless parent can be removed");

        DeviceId unchanged = 123;
        t.Check(registry.RegisterDevice(child, &unchanged) == DeviceStatus::NotFound, "registration rejects a removed parent");
        t.Equal(unchanged, 123, "failed parent lookup preserves output");
        t.Equal(registry.CountDevices(), 0, "failed registration publishes nothing");

        child.parent_id = CONTROLLER_DEVICE_ID;
        t.Check(registry.RegisterDevice(child, &unchanged) == DeviceStatus::InvalidArgument, "root marker is reserved for controller types");
    });

    suite.Run("resource/DeviceManager/state_and_driver", [&](TestCase& t)
    {
        DeviceManager registry;
        Device        device;
        DeviceId      id = INVALID_DEVICE_ID;
        device.operations = &DeviceTestOperations;

        if (!t.Check(registry.RegisterDevice(device, &id) == DeviceStatus::Success, "unbound device registered"))
        {
            return;
        }

        const DeviceState states[] = {
            DeviceState::INITIALIZING, DeviceState::INITIALIZED, DeviceState::FAILED,
            DeviceState::REMOVING, DeviceState::DISCOVERED
        };

        for (DeviceState state : states)
        {
            t.Check(registry.SetDeviceState(id, state) == DeviceStatus::Success, "valid state update succeeds");
            t.Check(registry.GetDevice(id, &device) == DeviceStatus::Success && device.state == state, "state update visible in snapshot");
        }

        t.Check(registry.SetDeviceState(id, static_cast<DeviceState>(-1)) == DeviceStatus::InvalidArgument, "invalid state rejected");
        t.Check(registry.AssociateDriver(id, INVALID_DRIVER_ID) == DeviceStatus::InvalidArgument, "invalid driver cannot be bound");
        t.Check(registry.AssociateDriver(id, 42) == DeviceStatus::Success, "opaque driver identifier bound");
        t.Check(registry.AssociateDriver(id, 42) == DeviceStatus::Success, "same driver binding is idempotent");
        t.Check(registry.AssociateDriver(id, 43) == DeviceStatus::Busy, "different driver cannot replace existing binding");
        t.Check(registry.UnregisterDevice(id) == DeviceStatus::Busy, "bound device cannot be removed");
        t.Check(registry.GetDevice(id, &device) == DeviceStatus::Success && device.driver_id == 42, "failed replacement preserves original driver");
        t.Check(registry.DisassociateDriver(id) == DeviceStatus::Success, "driver can be disassociated");
        t.Check(registry.DisassociateDriver(id) == DeviceStatus::Success, "disassociation is idempotent");
        t.Check(registry.GetDevice(id, &device) == DeviceStatus::Success && device.operations == &DeviceTestOperations, "binding does not change type-specific operations");
        t.Check(registry.AssociateDriver(id, 43) == DeviceStatus::Success, "replacement can bind after disassociation");
        t.Check(registry.DisassociateDriver(id) == DeviceStatus::Success, "replacement explicitly released");
        t.Check(registry.UnregisterDevice(id) == DeviceStatus::Success, "unbound device removed");
        t.Check(registry.SetDeviceState(id, DeviceState::FAILED) == DeviceStatus::NotFound, "stale state update rejected");
        t.Check(registry.AssociateDriver(id, 42) == DeviceStatus::NotFound, "stale driver association rejected");
        t.Check(registry.DisassociateDriver(id) == DeviceStatus::NotFound, "stale driver disassociation rejected");
    });

    suite.Run("resource/DeviceManager/enumeration_and_filters", [&](TestCase& t)
    {
        DeviceManager registry;
        Device        input;
        DeviceId      parent = INVALID_DEVICE_ID;
        DeviceId      first = INVALID_DEVICE_ID;
        DeviceId      second = INVALID_DEVICE_ID;

        input.type = DeviceType::CONTROLLER;
        input.parent_id = CONTROLLER_DEVICE_ID;
        t.Check(registry.RegisterDevice(input, &parent) == DeviceStatus::Success, "synthetic controller registered");

        input = {};
        input.bus = DeviceBus::PCI;
        input.parent_id = parent;
        t.Check(registry.RegisterDevice(input, &first) == DeviceStatus::Success, "first null identity registered");
        input.state = DeviceState::FAILED;
        t.Check(registry.RegisterDevice(input, &second) == DeviceStatus::Success, "distinct null identities allowed");

        DeviceEnumeration result;
        t.Check(registry.EnumerateDevices(nullptr, 0, &result) == DeviceStatus::Success, "count-only snapshot supported");
        t.Equal(result.total, 3, "count includes all records");
        t.Equal(result.copied, 0, "count-only call copies nothing");

        Device output[4];
        output[1].id = 123;
        t.Check(registry.EnumerateDevices(output, 1, &result) == DeviceStatus::Success, "short buffer is a successful partial snapshot");
        t.Equal(result.total, 3, "truncated copy still reports total");
        t.Equal(result.copied, 1, "copy respects buffer capacity");
        t.Equal(output[1].id, 123, "buffer beyond capacity untouched");

        DeviceFilter filter;
        filter.filter_bus = true;
        filter.bus = DeviceBus::PCI;
        t.Check(registry.EnumerateDevices(output, 4, &result, filter) == DeviceStatus::Success, "bus filtering succeeds");
        t.Equal(result.total, 2, "only PCI children matched");

        filter.filter_state = true;
        filter.state = DeviceState::FAILED;
        filter.filter_parent = true;
        filter.parent_id = parent;
        t.Check(registry.EnumerateDevices(output, 4, &result, filter) == DeviceStatus::Success, "combined filters succeed");
        t.Equal(result.total, 1, "filters intersect");
        t.Equal(output[0].id, second, "exact matching child copied");

        filter.parent_id = first;
        t.Check(registry.EnumerateDevices(output, 4, &result, filter) == DeviceStatus::Success, "empty filter succeeds");
        t.Equal(result.total, 0, "unmatched parent has no children");
        t.Equal(result.copied, 0, "no unmatched records copied");
    });

    suite.Run("resource/DeviceManager/invalid_arguments", [&](TestCase& t)
    {
        DeviceManager registry;
        Device        input;
        DeviceId      id = INVALID_DEVICE_ID;
        DeviceEnumeration result;

        t.Check(registry.RegisterDevice(input, nullptr) == DeviceStatus::InvalidArgument, "registration needs output ID");
        input.id = 1;
        t.Check(registry.RegisterDevice(input, &id) == DeviceStatus::InvalidArgument, "caller cannot assign IDs");
        input = {};
        input.driver_id = 1;
        t.Check(registry.RegisterDevice(input, &id) == DeviceStatus::InvalidArgument, "binding uses explicit association API");
        input = {};
        input.bus = static_cast<DeviceBus>(-1);
        t.Check(registry.RegisterDevice(input, &id) == DeviceStatus::InvalidArgument, "invalid bus rejected");
        input = {};
        input.type = static_cast<DeviceType>(-1);
        t.Check(registry.RegisterDevice(input, &id) == DeviceStatus::InvalidArgument, "invalid type rejected");
        input = {};
        input.state = static_cast<DeviceState>(-1);
        t.Check(registry.RegisterDevice(input, &id) == DeviceStatus::InvalidArgument, "invalid initial state rejected");
        t.Check(registry.GetDevice(1, nullptr) == DeviceStatus::InvalidArgument, "lookup needs output storage");
        t.Check(registry.EnumerateDevices(nullptr, 1, &result) == DeviceStatus::InvalidArgument, "nonzero capacity needs a buffer");
        t.Check(registry.EnumerateDevices(nullptr, 0, nullptr) == DeviceStatus::InvalidArgument, "enumeration needs a result");

        DeviceFilter filter;
        filter.filter_bus = true;
        filter.bus = static_cast<DeviceBus>(-1);
        t.Check(registry.EnumerateDevices(nullptr, 0, &result, filter) == DeviceStatus::InvalidArgument, "invalid bus filter rejected");
        filter = {};
        filter.filter_state = true;
        filter.state = static_cast<DeviceState>(-1);
        t.Check(registry.EnumerateDevices(nullptr, 0, &result, filter) == DeviceStatus::InvalidArgument, "invalid state filter rejected");
        t.Equal(registry.CountDevices(), 0, "invalid operations do not publish records");
    });
}

// Every CPU exercises the same live registry during the existing SMP phase.
// No case reporting here: AP outcomes are collected by the SMP runner.
bool exercise_device_registry(ResourceLayerCaps* resources)
{
    if (resources == nullptr || resources->deviceManager == nullptr)
    {
        return false;
    }

    DeviceManager* registry = resources->deviceManager;
    DeviceFilter filter;
    filter.filter_bus = true;
    filter.bus = DeviceBus::VIRTUAL;

    bool passed = true;

    for (size_t iteration = 0; iteration < 128; iteration++)
    {
        Device input;
        input.bus = DeviceBus::VIRTUAL;
        input.operations = &DeviceTestOperations;
        DeviceId id = INVALID_DEVICE_ID;

        if (registry->RegisterDevice(input, &id) != DeviceStatus::Success)
        {
            return false;
        }

        Device snapshot;
        Device output[4];
        DeviceEnumeration result;

        passed &= registry->GetDevice(id, &snapshot) == DeviceStatus::Success && snapshot.id == id;
        passed &= registry->SetDeviceState(id, DeviceState::INITIALIZED) == DeviceStatus::Success;
        passed &= registry->AssociateDriver(id, 42) == DeviceStatus::Success;
        passed &= registry->EnumerateDevices(output, 4, &result, filter) == DeviceStatus::Success && result.total >= 1;
        passed &= registry->DisassociateDriver(id) == DeviceStatus::Success;
        passed &= registry->UnregisterDevice(id) == DeviceStatus::Success;
        passed &= registry->GetDevice(id, &input) == DeviceStatus::NotFound;
        passed &= snapshot.id == id && snapshot.operations == &DeviceTestOperations;
    }

    return passed;
}
