#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>
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

    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint8_t  revision_id;
    uint8_t  prog_if;
    uint8_t  subclass;
    uint8_t  class_code;
    uint8_t  header_type;
    uint8_t  multifunction;
    uint8_t  capabilities_pointer;
    uint8_t  interrupt_line;
    uint8_t  interrupt_pin;
    uint8_t  bar_count;

    pci_bar_t bars[MAX_PCI_BARS];
} pci_device_t;

bool enumerate_devices(void);

#endif