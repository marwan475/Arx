#include <arch/arch.h>
#include <pci/pci.h>
#include <platform.h>

platform_pci_inventory_t platform_pci_enumerate(void* context)
{
    (void) context;

    if (platform.pci_status == PCI_NOT_ENUMERATED)
    {
        ARX_DEBUG_STORE(platform.debugStats.pci.outcome, ARX_PCI_ENUMERATING);
        platform.pci_status = arch_pci_enumerate();

        ARX_DEBUG_RUN(
            arx_pci_outcome_t outcome = ARX_PCI_FAILED;

            switch (platform.pci_status)
            {
                case PCI_SUCCESS:
                    outcome = ARX_PCI_SUCCEEDED;
                    break;
                case PCI_UNSUPPORTED:
                    outcome = ARX_PCI_UNSUPPORTED;
                    break;
                case PCI_NO_REGIONS:
                    outcome = ARX_PCI_NO_REGIONS;
                    break;
                default:
                    break;
            }

            ARX_DEBUG_STORE(platform.debugStats.pci.outcome, outcome);
        );
    }

    platform_pci_inventory_t inventory = {
        .devices   = platform.pci_devices,
        .count     = platform.pci_device_count,
        .status    = platform.pci_status,
        .truncated = platform.pci_truncated
    };

    return inventory;
}

const platform_pci_ops_t platform_pci_ops = {
    .context   = NULL,
    .enumerate = platform_pci_enumerate
};
