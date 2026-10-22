#pragma once

#include <stdint.h>

using DeviceId = uint64_t;
using DriverId = uint64_t;

constexpr DeviceId INVALID_DEVICE_ID    = 0;
constexpr DriverId INVALID_DRIVER_ID    = 0;
constexpr DeviceId CONTROLLER_DEVICE_ID = UINT64_MAX;

enum class DeviceBus
{
    PCI,
    USB,
    PLATFORM,
    VIRTUAL
};

enum class DeviceType
{
    UNKNOWN,
    CONTROLLER,
    STORAGE,
    NETWORK,
    DISPLAY,
    INPUT,
    AUDIO,
    SERIAL
};

enum class DeviceState
{
    DISCOVERED,
    INITIALIZING,
    INITIALIZED,
    FAILED,
    REMOVING
};

// Value snapshot, independent of every bus and operations-table layout.
// Non-null identity and operations storage is borrowed for the kernel lifetime,
// including after removal: copying this record does not retain either provider.
struct Device
{
    DeviceId    id         = INVALID_DEVICE_ID;
    DeviceId    parent_id  = INVALID_DEVICE_ID;
    DeviceBus   bus        = DeviceBus::PLATFORM;
    DeviceType  type       = DeviceType::UNKNOWN;
    const void* identity   = nullptr;
    const void* operations = nullptr;
    DeviceState state      = DeviceState::DISCOVERED;
    DriverId    driver_id  = INVALID_DRIVER_ID;
};
