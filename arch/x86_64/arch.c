#include <arch/arch.h>

void __attribute__((weak)) arch_map_page(uint64_t va, uint64_t pa, uint64_t flags, uintptr_t page_table)
{
    (void) va;
    (void) pa;
    (void) flags;
    (void) page_table;
}

void __attribute__((weak)) arch_unmap_page(uint64_t va, uintptr_t page_table)
{
    (void) va;
    (void) page_table;
}
