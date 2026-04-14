/*
 * acpi.h
 *
 * ACPI (Advanced Configuration and Power Interface) header
 *
 * - Provides functions to initialize and interact with ACPI tables
 *
 * Author: Marwan Mostafa
 *
 */

#ifndef ARX_KERNEL_ACPI_H
#define ARX_KERNEL_ACPI_H

#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>

void acpi_init(phys_addr_t rsdp_address);
uacpi_status acpi_get_madt(struct acpi_madt** out_madt, uacpi_table* out_table);
uacpi_status get_lapic_base_addr(struct acpi_madt* madt, uint64_t* out_lapic_base_addr);
uacpi_status get_lapics_from_mdat(struct acpi_madt* madt);

#endif