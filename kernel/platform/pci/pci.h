#ifndef ARX_PLATFORM_PCI_H
#define ARX_PLATFORM_PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_PCI_DEVICES 256
#define MAX_PCI_BARS 6

typedef struct pci_bar
{
    uint64_t base;
    uint32_t raw_low;
    uint32_t raw_high;
    uint8_t  present;
    uint8_t  is_io;
    uint8_t  is_64bit;
    uint8_t  prefetchable;
} pci_bar_t;

typedef struct pci_device
{
    uint16_t segment;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;

    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t revision_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type;
    uint8_t multifunction;
    uint8_t capabilities_pointer;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t bar_count;

    pci_bar_t bars[MAX_PCI_BARS];
} pci_device_t;

typedef enum pci_enumeration_status
{
    PCI_NOT_ENUMERATED,
    PCI_SUCCESS,
    PCI_UNSUPPORTED,
    PCI_NO_REGIONS,
    PCI_FAILED
} pci_enumeration_status_t;

// Borrowed boot snapshot. Records and their addresses remain valid for the
// kernel lifetime. BARs contain register metadata, not mapping extents.
typedef struct platform_pci_inventory
{
    const pci_device_t*       devices;
    size_t                    count;
    pci_enumeration_status_t  status;
    bool                      truncated;
} platform_pci_inventory_t;

typedef struct platform_pci_ops
{
    // The operation table is copied by PciManager; its context and returned
    // records must outlive all manager operations.
    void* context;
    platform_pci_inventory_t (*enumerate)(void* context);
} platform_pci_ops_t;

#ifdef __cplusplus
extern "C"
{
#endif

// BSP startup only, after memory, ACPI and architecture initialization. Repeated
// calls return the original result, including failures, without rescanning.
platform_pci_inventory_t platform_pci_enumerate(void* context);
extern const platform_pci_ops_t platform_pci_ops;

#ifdef __cplusplus
}
#endif

#endif
