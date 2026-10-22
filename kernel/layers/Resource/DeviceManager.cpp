#include "layers/Resource/DeviceManager.hpp"

#include <klib/klib.h>

static DeviceId NextDeviceId = 1;

static bool ValidBus(DeviceBus bus)
{
    return static_cast<unsigned>(bus) <= static_cast<unsigned>(DeviceBus::VIRTUAL);
}

static bool ValidType(DeviceType type)
{
    return static_cast<unsigned>(type) <= static_cast<unsigned>(DeviceType::SERIAL);
}

static bool ValidState(DeviceState state)
{
    return static_cast<unsigned>(state) <= static_cast<unsigned>(DeviceState::REMOVING);
}

static DeviceId AllocateDeviceId()
{
    DeviceId next = __atomic_load_n(&NextDeviceId, __ATOMIC_RELAXED);

    while (next != CONTROLLER_DEVICE_ID)
    {
        if (__atomic_compare_exchange_n(&NextDeviceId, &next, next + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        {
            return next;
        }
    }

    return INVALID_DEVICE_ID;
}

DeviceManager::DeviceManager()
    : Lock(0), Devices(nullptr), DeviceCount(0)
{
}

DeviceManager::~DeviceManager()
{
    while (Devices != nullptr)
    {
        DeviceRecord* record = Devices;
        Devices = record->next;

        kfree(record);
    }
}

DeviceManager::DeviceRecord* DeviceManager::FindRecord(DeviceId id) const
{
    for (DeviceRecord* record = Devices; record != nullptr; record = record->next)
    {
        if (record->device.id == id)
        {
            return record;
        }
    }

    return nullptr;
}

DeviceStatus DeviceManager::RegisterDevice(const Device& device, DeviceId* id)
{
    if (id == nullptr || device.id != INVALID_DEVICE_ID || device.driver_id != INVALID_DRIVER_ID ||
        !ValidBus(device.bus) || !ValidType(device.type) || !ValidState(device.state) ||
        (device.parent_id == CONTROLLER_DEVICE_ID && device.type != DeviceType::CONTROLLER))
    {
        return DeviceStatus::InvalidArgument;
    }

    DeviceRecord* record = static_cast<DeviceRecord*>(kzalloc(sizeof(DeviceRecord)));

    if (record == nullptr)
    {
        return DeviceStatus::NoMemory;
    }

    record->device = device;

    spinlock_acquire_preempt(&Lock);

    DeviceStatus status = DeviceStatus::Success;

    if (device.parent_id != INVALID_DEVICE_ID && device.parent_id != CONTROLLER_DEVICE_ID && FindRecord(device.parent_id) == nullptr)
    {
        status = DeviceStatus::NotFound;
    }

    if (status == DeviceStatus::Success && device.identity != nullptr)
    {
        for (DeviceRecord* existing = Devices; existing != nullptr; existing = existing->next)
        {
            if (existing->device.bus == device.bus && existing->device.identity == device.identity)
            {
                status = DeviceStatus::AlreadyExists;
                break;
            }
        }
    }

    if (status == DeviceStatus::Success)
    {
        record->device.id = AllocateDeviceId();

        if (record->device.id == INVALID_DEVICE_ID)
        {
            status = DeviceStatus::IdExhausted;
        }
    }

    if (status == DeviceStatus::Success)
    {
        record->next = Devices;
        Devices = record;
        DeviceCount++;
        *id = record->device.id;
    }

    spinlock_release_preempt(&Lock);

    if (status != DeviceStatus::Success)
    {
        kfree(record);
    }

    return status;
}

DeviceStatus DeviceManager::UnregisterDevice(DeviceId id)
{
    spinlock_acquire_preempt(&Lock);

    DeviceRecord** link = &Devices;

    while (*link != nullptr && (*link)->device.id != id)
    {
        link = &(*link)->next;
    }

    DeviceRecord* record = *link;
    DeviceStatus  status = DeviceStatus::Success;

    if (record == nullptr)
    {
        status = DeviceStatus::NotFound;
    }
    else if (record->device.driver_id != INVALID_DRIVER_ID)
    {
        status = DeviceStatus::Busy;
    }
    else
    {
        for (DeviceRecord* child = Devices; child != nullptr; child = child->next)
        {
            if (child->device.parent_id == id)
            {
                status = DeviceStatus::Busy;
                break;
            }
        }
    }

    if (status == DeviceStatus::Success)
    {
        *link = record->next;
        DeviceCount--;
    }

    spinlock_release_preempt(&Lock);

    if (status == DeviceStatus::Success)
    {
        kfree(record);
    }

    return status;
}

DeviceStatus DeviceManager::GetDevice(DeviceId id, Device* device) const
{
    if (device == nullptr)
    {
        return DeviceStatus::InvalidArgument;
    }

    spinlock_acquire_preempt(&Lock);

    DeviceRecord* record = FindRecord(id);
    DeviceStatus  status = DeviceStatus::NotFound;

    if (record != nullptr)
    {
        *device = record->device;
        status = DeviceStatus::Success;
    }

    spinlock_release_preempt(&Lock);

    return status;
}

DeviceStatus DeviceManager::EnumerateDevices(Device* devices, size_t capacity, DeviceEnumeration* result,
                                             const DeviceFilter& filter) const
{
    if (result == nullptr || (capacity != 0 && devices == nullptr) ||
        (filter.filter_bus && !ValidBus(filter.bus)) || (filter.filter_state && !ValidState(filter.state)))
    {
        return DeviceStatus::InvalidArgument;
    }

    DeviceEnumeration enumeration;

    spinlock_acquire_preempt(&Lock);

    for (DeviceRecord* record = Devices; record != nullptr; record = record->next)
    {
        const Device& device = record->device;

        if ((filter.filter_bus && device.bus != filter.bus) ||
            (filter.filter_state && device.state != filter.state) ||
            (filter.filter_parent && device.parent_id != filter.parent_id))
        {
            continue;
        }

        if (enumeration.copied < capacity)
        {
            devices[enumeration.copied++] = device;
        }

        enumeration.total++;
    }

    *result = enumeration;

    spinlock_release_preempt(&Lock);

    return DeviceStatus::Success;
}

size_t DeviceManager::CountDevices() const
{
    spinlock_acquire_preempt(&Lock);

    size_t count = DeviceCount;

    spinlock_release_preempt(&Lock);

    return count;
}

DeviceStatus DeviceManager::SetDeviceState(DeviceId id, DeviceState state)
{
    if (!ValidState(state))
    {
        return DeviceStatus::InvalidArgument;
    }

    spinlock_acquire_preempt(&Lock);

    DeviceRecord* record = FindRecord(id);
    DeviceStatus  status = DeviceStatus::NotFound;

    if (record != nullptr)
    {
        record->device.state = state;
        status = DeviceStatus::Success;
    }

    spinlock_release_preempt(&Lock);

    return status;
}

DeviceStatus DeviceManager::AssociateDriver(DeviceId id, DriverId driverId)
{
    if (driverId == INVALID_DRIVER_ID)
    {
        return DeviceStatus::InvalidArgument;
    }

    spinlock_acquire_preempt(&Lock);

    DeviceRecord* record = FindRecord(id);
    DeviceStatus  status = DeviceStatus::NotFound;

    if (record != nullptr)
    {
        if (record->device.driver_id != INVALID_DRIVER_ID && record->device.driver_id != driverId)
        {
            status = DeviceStatus::Busy;
        }
        else
        {
            record->device.driver_id = driverId;
            status = DeviceStatus::Success;
        }
    }

    spinlock_release_preempt(&Lock);

    return status;
}

DeviceStatus DeviceManager::DisassociateDriver(DeviceId id)
{
    spinlock_acquire_preempt(&Lock);

    DeviceRecord* record = FindRecord(id);
    DeviceStatus  status = DeviceStatus::NotFound;

    if (record != nullptr)
    {
        record->device.driver_id = INVALID_DRIVER_ID;
        status = DeviceStatus::Success;
    }

    spinlock_release_preempt(&Lock);

    return status;
}
