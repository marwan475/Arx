/*
 * acpi.c
 *
 * ACPI (Advanced Configuration and Power Interface) implementation
 *
 * - uACPI port
 *
 * Author: Marwan Mostafa
 *
 */

#include <acpi/acpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/log.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>

#define ACPI_EARLY_TABLE_BUFFER_SIZE 4096

uacpi_phys_addr uacpi_rsdp_address;
void*           uacpi_early_table_buffer;

void acpi_init(phys_addr_t rsdp_address)
{
    uacpi_status status;

    if (dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present && rsdp_address >= dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset)
    {
        rsdp_address = hhdm_to_pa(rsdp_address, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);
    }

    uacpi_rsdp_address = (uacpi_phys_addr) rsdp_address;

    uacpi_early_table_buffer = pmm_alloc(ACPI_EARLY_TABLE_BUFFER_SIZE);

    status = uacpi_setup_early_table_access(uacpi_early_table_buffer, ACPI_EARLY_TABLE_BUFFER_SIZE);
    if (status != UACPI_STATUS_OK)
    {
        kprintf("ACPI: uACPI early init failed: %s (%u)\n", uacpi_status_to_string(status), (unsigned) status);
        return;
    }

    kprintf("ACPI: uACPI barebones early table access initialized\n");
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address)
{
    if (uacpi_rsdp_address == 0)
    {
        *out_rsdp_address = 0;
        return UACPI_STATUS_NOT_FOUND;
    }

    *out_rsdp_address = uacpi_rsdp_address;
    return UACPI_STATUS_OK;
}

void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    uint64_t aligned_pa  = align_down((uint64_t) addr, PAGE_SIZE);
    uint64_t offset      = (uint64_t) addr - aligned_pa;
    uint64_t aligned_len = align_up((uint64_t) len + offset, PAGE_SIZE);

    virt_addr_t va = vmm_reserve_region(dispatcher.cpus[arch_cpu_id()].address_space, aligned_len, VIRT_ADDR_KERNEL);
    if (va == 0)
    {
        return NULL;
    }

    uint64_t map_flags = 0;
    ARCH_PAGE_FLAGS_INIT(map_flags);
    ARCH_PAGE_FLAG_SET_READ(map_flags);
    ARCH_PAGE_FLAG_SET_WRITE(map_flags);

    vmm_map_range(va, aligned_pa, aligned_len, map_flags, dispatcher.cpus[arch_cpu_id()].address_space);
    return (void*) (va + offset);
}

void uacpi_kernel_unmap(void* addr, uacpi_size len)
{
    uint64_t aligned_va  = align_down((uint64_t) addr, PAGE_SIZE);
    uint64_t offset      = (uint64_t) addr - aligned_va;
    uint64_t aligned_len = align_up((uint64_t) len + offset, PAGE_SIZE);

    vmm_unmap_range((virt_addr_t) aligned_va, aligned_len, dispatcher.cpus[arch_cpu_id()].address_space);
    vmm_free_region(dispatcher.cpus[arch_cpu_id()].address_space, (virt_addr_t) aligned_va);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* text)
{
    kprintf("uACPI[%u]: %s", (unsigned) level, (const char*) text);
}