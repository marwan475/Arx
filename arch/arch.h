#ifndef ARCH_H
#define ARCH_H

#include <stdint.h>

#if defined(__x86_64__)
#define ARCH_PAGE_FLAGS_INIT(flags) ((flags) = 0ULL)

#define ARCH_PAGE_FLAG_SET_WRITABLE(flags) ((flags) |= (1ULL << 1))
#define ARCH_PAGE_FLAG_SET_USER(flags) ((flags) |= (1ULL << 2))
#define ARCH_PAGE_FLAG_SET_WRITE_THROUGH(flags) ((flags) |= (1ULL << 3))
#define ARCH_PAGE_FLAG_SET_CACHE_DISABLE(flags) ((flags) |= (1ULL << 4))
#define ARCH_PAGE_FLAG_SET_ACCESSED(flags) ((flags) |= (1ULL << 5))
#define ARCH_PAGE_FLAG_SET_DIRTY(flags) ((flags) |= (1ULL << 6))
#define ARCH_PAGE_FLAG_SET_PAT(flags) ((flags) |= (1ULL << 7))
#define ARCH_PAGE_FLAG_SET_GLOBAL(flags) ((flags) |= (1ULL << 8))
#define ARCH_PAGE_FLAG_SET_NO_EXECUTE(flags) ((flags) |= (1ULL << 63))
#elif defined(__aarch64__)
#define ARCH_PAGE_FLAGS_INIT(flags) ((flags) = 0ULL)

#define ARCH_PAGE_FLAG_SET_ATTRINDX(flags, index) ((flags) = ((flags) & ~(0x7ULL << 2)) | ((((uint64_t) (index)) & 0x7ULL) << 2))
#define ARCH_PAGE_FLAG_SET_NS(flags) ((flags) |= (1ULL << 5))
#define ARCH_PAGE_FLAG_SET_USER(flags) ((flags) |= (1ULL << 6))
#define ARCH_PAGE_FLAG_SET_READ_ONLY(flags) ((flags) |= (1ULL << 7))
#define ARCH_PAGE_FLAG_SET_SH_INNER(flags) ((flags) = ((flags) & ~(3ULL << 8)) | (3ULL << 8))
#define ARCH_PAGE_FLAG_SET_SH_OUTER(flags) ((flags) = ((flags) & ~(3ULL << 8)) | (2ULL << 8))
#define ARCH_PAGE_FLAG_SET_AF(flags) ((flags) |= (1ULL << 10))
#define ARCH_PAGE_FLAG_SET_NG(flags) ((flags) |= (1ULL << 11))
#define ARCH_PAGE_FLAG_SET_DBM(flags) ((flags) |= (1ULL << 51))
#define ARCH_PAGE_FLAG_SET_CONTIGUOUS(flags) ((flags) |= (1ULL << 52))
#define ARCH_PAGE_FLAG_SET_PXN(flags) ((flags) |= (1ULL << 53))
#define ARCH_PAGE_FLAG_SET_UXN(flags) ((flags) |= (1ULL << 54))
#define ARCH_PAGE_FLAG_SET_NO_EXECUTE(flags)                                                                                     \
	do                                                                                                                           \
	{                                                                                                                            \
		ARCH_PAGE_FLAG_SET_PXN(flags);                                                                                          \
		ARCH_PAGE_FLAG_SET_UXN(flags);                                                                                          \
	} while (0)
#else
#error Unsupported architecture
#endif

void arch_halt(void);
void arch_pause(void);

// Page table should be raw physical address
void arch_map_page(uint64_t va, uint64_t pa, uint64_t flags, uintptr_t page_table);
void arch_unmap_page(uint64_t va, uintptr_t page_table);

// returns/takes physical address of page table
uintptr_t arch_get_pt(void);
void arch_set_pt(uintptr_t pt);

// Optimization for mapping/unmapping large ranges of pages, to avoid redundant page table walks and TLB clears
void arch_map_range(uint64_t va_start, uint64_t pa_start, uint64_t size, uint64_t flags, uintptr_t page_table);
void arch_unmap_range(uint64_t va_start, uint64_t size, uintptr_t page_table);

// Update page flags
void arch_protect(uint64_t va, uint64_t flags, uintptr_t page_table);
void arch_protect_range(uint64_t va_start, uint64_t size, uint64_t flags, uintptr_t page_table);

#endif