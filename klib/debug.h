#ifndef KLIB_DEBUG_H
#define KLIB_DEBUG_H

#include <boot/boot.h>
#include <stdint.h>

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

#endif
