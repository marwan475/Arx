#include <acpi/acpi.h>

void arch_acpi_flush_cpu_cache(void)
{
    // Runtime initialization is unsupported on this target.
}

uacpi_status arch_acpi_init(struct acpi_madt* madt)
{
    (void) madt;
    return UACPI_STATUS_UNIMPLEMENTED;
}

bool arch_acpi_io_read(uint16_t port, uint8_t width, uint32_t* value)
{
    (void) port;
    (void) width;
    (void) value;
    return false;
}

bool arch_acpi_io_write(uint16_t port, uint8_t width, uint32_t value)
{
    (void) port;
    (void) width;
    (void) value;
    return false;
}

bool arch_acpi_pci_access(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t width, bool write, uint32_t* value)
{
    (void) segment;
    (void) bus;
    (void) device;
    (void) function;
    (void) offset;
    (void) width;
    (void) write;
    (void) value;
    return false;
}

uint32_t arch_acpi_route_interrupt(uint32_t irq)
{
    (void) irq;
    return 0;
}

void arch_acpi_mask_interrupt(uint32_t vector, bool masked)
{
    (void) vector;
    (void) masked;
}

void arch_bootstrap_switch_context(struct arch_task_context* outgoing, const struct arch_task_context* incoming)
{
    (void) outgoing;
    (void) incoming;
}
