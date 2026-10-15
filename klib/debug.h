#ifndef KLIB_DEBUG_H
#define KLIB_DEBUG_H

#include <boot/boot.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    int kterm_printf(const char* format, ...);

#ifndef DEBUG
#define DEBUG 0
#endif

#if DEBUG
#define KDEBUG(fmt, ...) kterm_printf("\x1b[32m[debug]\x1b[0m " fmt, ##__VA_ARGS__)
#else
#define KDEBUG(...) ((void) 0)
#endif

    void debug_validate_boot(const struct boot_info* boot_info, uint64_t cpu_count);
    void debug_pci_devices(void);

#if BOOT_DIAGNOSTICS
    void debug_boot_checkpoint(const char* operation);
    void debug_boot_tick(const void* interrupted_pc);
#define BOOT_CHECKPOINT(operation) debug_boot_checkpoint(operation)
#else
#define BOOT_CHECKPOINT(operation) ((void) 0)
#endif

#ifdef __cplusplus
}
#endif

#endif
