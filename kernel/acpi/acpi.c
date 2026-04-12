#include <acpi/acpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/log.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>

#define ACPI_EARLY_TABLE_BUFFER_SIZE 4096

static uacpi_phys_addr g_rsdp_address;
static uint8_t         g_early_table_buffer[ACPI_EARLY_TABLE_BUFFER_SIZE] __attribute__((aligned(sizeof(uintptr_t))));

void acpi_init(phys_addr_t rsdp_address)
{
    uacpi_status status;

    g_rsdp_address = (uacpi_phys_addr) rsdp_address;

    status = uacpi_setup_early_table_access(g_early_table_buffer, sizeof(g_early_table_buffer));
    if (status != UACPI_STATUS_OK)
    {
        kprintf("ACPI: uACPI early init failed: %s (%u)\n", uacpi_status_to_string(status), (unsigned) status);
        return;
    }

    kprintf("ACPI: uACPI barebones early table access initialized\n");
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address)
{
    if (out_rsdp_address == 0)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    if (g_rsdp_address == 0)
    {
        *out_rsdp_address = 0;
        return UACPI_STATUS_NOT_FOUND;
    }

    *out_rsdp_address = g_rsdp_address;
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