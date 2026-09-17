#include <arch/arch.h>
#include <device/device.h>

bool enumerate_devices(void)
{
    return arch_device_init();
}