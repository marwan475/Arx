#include <klib/klib.h>
#include <stdint.h>
#include <uacpi/kernel_api.h>
#include <uacpi/log.h>
#include <uacpi/status.h>

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address)
{
    if (out_rsdp_address == 0)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    // ACPI handoff is not wired yet; this keeps integration buildable.
    *out_rsdp_address = 0;
    return UACPI_STATUS_NOT_FOUND;
}

void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    (void) len;

    // Temporary identity mapping for integration-only builds.
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
