#include <arch/arch.h>
#include <device/device.h>
#include <platform.h>

bool enumerate_devices(void)
{
    ARX_DEBUG_STORE(platform.debugStats.pci.outcome, ARX_PCI_ENUMERATING);
    bool result = arch_device_init();

    ARX_DEBUG_RUN(
        if (!result)
        {
            ARX_DEBUG_STORE(platform.debugStats.pci.outcome, ARX_PCI_FAILED);
        }
        else if (platform.debugStats.pci.outcome == ARX_PCI_ENUMERATING)
        {
            ARX_DEBUG_STORE(platform.debugStats.pci.outcome, ARX_PCI_SUCCEEDED);
        }
    );

    return result;
}
