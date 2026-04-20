#include <device/device.h>
#include <arch/arch.h>

bool enumerate_devices(void)
{
	return arch_device_init();
}