#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>

void arch_halt(void);
void arch_pause(void);

void arch_map_page(uint64_t va, uint64_t pa, uint64_t flags, uint64_t page_table);
void arch_unmap_page(uint64_t va, uint64_t page_table);

#endif