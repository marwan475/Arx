#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_PCI_DEVICES 256

typedef struct pci_device
{
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
} pci_device_t;

bool enumerate_devices(void);

#endif