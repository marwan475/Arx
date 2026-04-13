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

void acpi_init(phys_addr_t rsdp_address);

#endif