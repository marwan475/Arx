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

    uacpi_rsdp_address = (uacpi_phys_addr) rsdp_address;

    uacpi_early_table_buffer = pmm_alloc(&pmm_zone, ACPI_EARLY_TABLE_BUFFER_SIZE);

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
    (void) len;

    if (pmm_zone.hhdm_present)
    {
        return (void*) (uintptr_t) (addr + pmm_zone.hhdm_offset);
    }

    return (void*) (uintptr_t) addr;
}

void uacpi_kernel_unmap(void* addr, uacpi_size len)
{
    (void) addr;
    (void) len;
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* text)
{
    kprintf("uACPI[%u]: %s", (unsigned) level, (const char*) text);
}