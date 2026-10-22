#pragma once

#ifdef __cplusplus
extern "C"
{
#endif
    void arch_acpi_flush_cpu_cache(void);
#ifdef __cplusplus
}
#endif

#define UACPI_ARCH_FLUSH_CPU_CACHE() arch_acpi_flush_cpu_cache()
