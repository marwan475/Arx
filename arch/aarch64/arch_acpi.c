#include <acpi/acpi.h>

uacpi_status arch_acpi_init(struct acpi_madt* madt)
{
    (void) madt;
    return UACPI_STATUS_OK;
}
