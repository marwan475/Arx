#pragma once

#include <acpi/runtime.h>
#include <uacpi/kernel_api.h>

#ifdef __cplusplus
extern "C"
{
#endif

acpi_status_t acpi_host_initialize(void);
bool acpi_host_can_call(void);
void acpi_host_yield(void);
void acpi_host_wake(uint64_t* sequence);
bool acpi_host_wait(const uint64_t* sequence, uint64_t expected, uint64_t deadline);
bool acpi_host_dispatch_interrupt(uint32_t vector);
void acpi_host_publish_event(const acpi_event_t* event);
acpi_status_t acpi_host_status(uacpi_status status);
void acpi_runtime_host_failed(void);

#ifdef __cplusplus
}
#endif

