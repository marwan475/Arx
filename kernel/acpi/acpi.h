#ifndef ARX_KERNEL_ACPI_H
#define ARX_KERNEL_ACPI_H

#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>

uacpi_status acpi_get_madt(struct acpi_madt** out_madt, uacpi_table* out_table);
uacpi_status acpi_get_mcfg(struct acpi_mcfg** out_mcfg, uacpi_table* out_table);

void acpi_init(phys_addr_t rsdp_address);

#endif