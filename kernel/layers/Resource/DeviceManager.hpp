#pragma once

#include "layers/Resource/Device.hpp"

#include <klib/spinlock.h>
#include <stddef.h>

enum class DeviceStatus
{
    Success,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    NoMemory,
    Busy,
    IdExhausted,
    NotReady
};

struct DeviceFilter
{
    bool        filter_bus    = false;
    DeviceBus   bus           = DeviceBus::PLATFORM;
    bool        filter_state  = false;
    DeviceState state         = DeviceState::DISCOVERED;
    bool        filter_parent = false;
    DeviceId    parent_id     = INVALID_DEVICE_ID;
};

struct DeviceEnumeration
{
    size_t copied = 0;
    size_t total  = 0;
};

class DeviceManager
{
public:
    DeviceManager();
    // Callers must quiesce first. Frees records, never identities or operations.
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // Startup/task context only, not interrupt context. Every operation is locked.
    // Registration copies its input; id and driver_id must initially be invalid.
    // Parent, type, bus, identity and operations cannot change after registration.
    // Failures leave output arguments unchanged. IDs are never reused.
    DeviceStatus RegisterDevice(const Device& device, DeviceId* id);
    // Rejects removal while children or a bound driver remain.
    DeviceStatus UnregisterDevice(DeviceId id);
    DeviceStatus GetDevice(DeviceId id, Device* device) const;

    // One coherent snapshot per call, in unspecified order. A null buffer is
    // allowed only with zero capacity. total includes matches beyond capacity.
    DeviceStatus EnumerateDevices(Device* devices, size_t capacity, DeviceEnumeration* result,
                                  const DeviceFilter& filter = {}) const;
    size_t CountDevices() const;

    // Valid states may be assigned without an enforced transition graph.
    DeviceStatus SetDeviceState(DeviceId id, DeviceState state);
    DeviceStatus AssociateDriver(DeviceId id, DriverId driverId);
    DeviceStatus DisassociateDriver(DeviceId id);

private:
    struct DeviceRecord
    {
        Device        device;
        DeviceRecord* next;
    };

    // Lock must be held by the caller.
    DeviceRecord* FindRecord(DeviceId id) const;

    mutable spinlock_t Lock;
    DeviceRecord*      Devices;
    size_t             DeviceCount;
};
