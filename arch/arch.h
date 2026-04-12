/*
 * arch.h
 *
 * This header defines architecture specific handlers 
 *
 * - Common architecture interface
 *
 * Author: Marwan Mostafa
 *
 */

#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>

#ifndef PHYS_ADDR_T_DEFINED
#define PHYS_ADDR_T_DEFINED
typedef uint64_t phys_addr_t;
#endif

#ifndef VIRT_ADDR_T_DEFINED
#define VIRT_ADDR_T_DEFINED
typedef uint64_t virt_addr_t;
#endif

#if defined(__x86_64__)
#define ARCH_PAGE_FLAGS_INIT(flags) ((flags) = (1ULL << 63))

#define ARCH_PAGE_FLAG_SET_READ(flags) ((void) (flags))
#define ARCH_PAGE_FLAG_SET_WRITE(flags) ((flags) |= (1ULL << 1))
#define ARCH_PAGE_FLAG_SET_EXEC(flags) ((flags) &= ~(1ULL << 63))
#define ARCH_PAGE_FLAG_SET_USER(flags) ((flags) |= (1ULL << 2))
#define ARCH_PAGE_FLAG_SET_GLOBAL(flags) ((flags) |= (1ULL << 8))
#define ARCH_PAGE_FLAG_SET_NOCACHE(flags) ((flags) |= (1ULL << 4))
#define ARCH_PAGE_FLAG_SET_WRITETHROUGH(flags) ((flags) |= (1ULL << 3))
#elif defined(__aarch64__)
#define AARCH64_PAGE_FLAG_ATTRINDX_MASK (0x7ULL << 2)

#define ARCH_PAGE_FLAGS_INIT(flags) ((flags) = (1ULL << 10) | (3ULL << 8) | (1ULL << 53) | (1ULL << 54))

#define ARCH_PAGE_FLAG_SET_READ(flags) ((void) (flags))
#define ARCH_PAGE_FLAG_SET_WRITE(flags) ((flags) &= ~(1ULL << 7))
#define ARCH_PAGE_FLAG_SET_EXEC(flags) ((flags) &= ~((1ULL << 53) | (1ULL << 54)))
#define ARCH_PAGE_FLAG_SET_USER(flags) ((flags) |= (1ULL << 6) | (1ULL << 11))
#define ARCH_PAGE_FLAG_SET_GLOBAL(flags) ((flags) &= ~(1ULL << 11))
#define ARCH_PAGE_FLAG_SET_NOCACHE(flags) ((flags) = ((flags) & ~AARCH64_PAGE_FLAG_ATTRINDX_MASK) | (1ULL << 2))
#define ARCH_PAGE_FLAG_SET_WRITETHROUGH(flags) ((flags) = ((flags) & ~AARCH64_PAGE_FLAG_ATTRINDX_MASK) | (2ULL << 2))
#else
#error Unsupported architecture
#endif

void arch_halt(void);
void arch_pause(void);

// Page table should be raw physical address
void arch_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, phys_addr_t page_table);
void arch_unmap_page(virt_addr_t va, phys_addr_t page_table);

// returns/takes physical address of page table
phys_addr_t arch_get_pt(void);
void        arch_set_pt(phys_addr_t pt);

// Optimization for mapping/unmapping large ranges of pages, to avoid redundant page table walks and cache clears
void arch_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, phys_addr_t page_table);
void arch_unmap_range(virt_addr_t va_start, uint64_t size, phys_addr_t page_table);

// Update page flags
void arch_protect(virt_addr_t va, uint64_t flags, phys_addr_t page_table);
void arch_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, phys_addr_t page_table);

// Returns physical address for a mapped virtual address, or 0 if unmapped.
phys_addr_t arch_virt_to_phys(virt_addr_t va, phys_addr_t page_table);

#endif
