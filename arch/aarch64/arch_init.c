#include <arch/arch.h>
#include <platform.h>

bool arch_init(void)
{
    return false;
}

bool arch_device_init(void)
{
    return false;
}

void arch_init_cpu_info(void)
{
    platform.arch = ARCH_AARCH64;
}

void arch_boot_cpu_hardware_ids(const struct boot_info* boot)
{
    (void) boot;
}

void arch_calibrate_scheduler_timer(void)
{
}

void arch_init_platform_dma(void)
{
}
