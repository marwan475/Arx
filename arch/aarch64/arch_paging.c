/*
 * arch_paging.c
 *
 * AArch64 architecture-specific paging implementation
 *
 * - Provides functions for managing page tables and paging operations
 * - Optimizations for working with large ranges of pages
 *      - Avoiding redundant page table walks/cache flushes and batching flushes
 *
 * Author: Marwan Mostafa
 *
 */

#include <arch/arch.h>
#include <memory/pmm.h>
#include <memory/vmm.h>

#define AARCH64_PT_LEVEL_BITS 9ULL
#define AARCH64_PT_ENTRIES (1ULL << AARCH64_PT_LEVEL_BITS)
#define AARCH64_PT_INDEX_MASK (AARCH64_PT_ENTRIES - 1ULL)

#define AARCH64_PT_SHIFT_L3 PAGE_SHIFT
#define AARCH64_PT_SHIFT_L2 (AARCH64_PT_SHIFT_L3 + AARCH64_PT_LEVEL_BITS)
#define AARCH64_PT_SHIFT_L1 (AARCH64_PT_SHIFT_L2 + AARCH64_PT_LEVEL_BITS)
#define AARCH64_PT_SHIFT_L0 (AARCH64_PT_SHIFT_L1 + AARCH64_PT_LEVEL_BITS)

#define AARCH64_PTE_BIT_VALID 0ULL
#define AARCH64_PTE_BIT_TABLE_OR_PAGE 1ULL
#define AARCH64_PTE_BIT_ATTRINDX_0 2ULL
#define AARCH64_PTE_BIT_ATTRINDX_1 3ULL
#define AARCH64_PTE_BIT_ATTRINDX_2 4ULL
#define AARCH64_PTE_BIT_NS 5ULL
#define AARCH64_PTE_BIT_AP_0 6ULL
#define AARCH64_PTE_BIT_AP_1 7ULL
#define AARCH64_PTE_BIT_SH_0 8ULL
#define AARCH64_PTE_BIT_SH_1 9ULL
#define AARCH64_PTE_BIT_AF 10ULL
#define AARCH64_PTE_BIT_nG 11ULL
#define AARCH64_PTE_BIT_DBM 51ULL
#define AARCH64_PTE_BIT_CONTIGUOUS 52ULL
#define AARCH64_PTE_BIT_PXN 53ULL
#define AARCH64_PTE_BIT_UXN 54ULL

#define AARCH64_PTE_VALID (1ULL << AARCH64_PTE_BIT_VALID)
#define AARCH64_PTE_TABLE_OR_PAGE (1ULL << AARCH64_PTE_BIT_TABLE_OR_PAGE)

#define AARCH64_PHYS_ADDR_BITS 48ULL
#define AARCH64_PAGE_OFFSET_MASK ((1ULL << PAGE_SHIFT) - 1ULL)
#define AARCH64_PHYS_ADDR_MASK ((1ULL << AARCH64_PHYS_ADDR_BITS) - 1ULL)
#define AARCH64_PTE_ADDR_MASK (AARCH64_PHYS_ADDR_MASK & ~AARCH64_PAGE_OFFSET_MASK)

#define AARCH64_CANONICAL_VA_BITS 48ULL
#define AARCH64_CANONICAL_LOW_MASK ((1ULL << AARCH64_CANONICAL_VA_BITS) - 1ULL)
#define AARCH64_CANONICAL_HIGH_MASK (~AARCH64_CANONICAL_LOW_MASK)

#define AARCH64_PTE_ALLOWED_MAP_FLAGS                                                                                                                                                                                                                                                                      \
    ((1ULL << AARCH64_PTE_BIT_ATTRINDX_0) | (1ULL << AARCH64_PTE_BIT_ATTRINDX_1) | (1ULL << AARCH64_PTE_BIT_ATTRINDX_2) | (1ULL << AARCH64_PTE_BIT_NS) | (1ULL << AARCH64_PTE_BIT_AP_0) | (1ULL << AARCH64_PTE_BIT_AP_1) | (1ULL << AARCH64_PTE_BIT_SH_0) | (1ULL << AARCH64_PTE_BIT_SH_1)                 \
     | (1ULL << AARCH64_PTE_BIT_AF) | (1ULL << AARCH64_PTE_BIT_nG) | (1ULL << AARCH64_PTE_BIT_DBM) | (1ULL << AARCH64_PTE_BIT_CONTIGUOUS) | (1ULL << AARCH64_PTE_BIT_PXN) | (1ULL << AARCH64_PTE_BIT_UXN))

static inline void aarch64_dsb_ishst(void)
{
    __asm__ volatile("dsb ishst" : : : "memory");
}

static inline void aarch64_dsb_ish(void)
{
    __asm__ volatile("dsb ish" : : : "memory");
}

static inline void aarch64_isb(void)
{
    __asm__ volatile("isb" : : : "memory");
}

static inline void aarch64_tlbi_vaael1is(virt_addr_t va)
{
    const uint64_t tlbi_operand = va >> PAGE_SHIFT;
    __asm__ volatile("tlbi vaae1is, %0" : : "r"(tlbi_operand) : "memory");
}

static inline void aarch64_tlbi_vmalle1is(void)
{
    __asm__ volatile("tlbi vmalle1is" : : : "memory");
}

static inline void aarch64_sync_current_page_table(void)
{
    aarch64_dsb_ishst();
    aarch64_tlbi_vmalle1is();
    aarch64_dsb_ish();
    aarch64_isb();
}

static bool aarch64_is_page_aligned(uint64_t value);
static bool aarch64_is_pa_encodable(phys_addr_t pa);

static inline uint64_t aarch64_read_ttbr1_el1(void)
{
    uint64_t ttbr1 = 0;
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
    return ttbr1;
}

static inline void aarch64_write_ttbr1_el1(uint64_t ttbr1)
{
    __asm__ volatile("msr ttbr1_el1, %0" : : "r"(ttbr1) : "memory");
}

phys_addr_t arch_get_pt(void)
{
    return (uintptr_t) (aarch64_read_ttbr1_el1() & AARCH64_PTE_ADDR_MASK);
}

void arch_set_pt(phys_addr_t pt)
{
    if (pt == 0)
    {
        kprintf("Arx kernel: arch_set_pt rejected null page table\n");
        return;
    }

    if (!aarch64_is_page_aligned((uint64_t) pt))
    {
        kprintf("Arx kernel: arch_set_pt rejected unaligned page table\n");
        return;
    }

    if (!aarch64_is_pa_encodable((uint64_t) pt))
    {
        kprintf("Arx kernel: arch_set_pt rejected non-encodable page table\n");
        return;
    }

    const uint64_t old_ttbr1 = aarch64_read_ttbr1_el1();
    const uint64_t new_ttbr1 = (old_ttbr1 & ~AARCH64_PTE_ADDR_MASK) | (((uint64_t) pt) & AARCH64_PTE_ADDR_MASK);
    const uint64_t old_baddr = old_ttbr1 & AARCH64_PTE_ADDR_MASK;
    const uint64_t new_baddr = new_ttbr1 & AARCH64_PTE_ADDR_MASK;
    if (old_baddr == new_baddr)
    {
        return;
    }

    aarch64_dsb_ishst();
    aarch64_tlbi_vmalle1is();
    aarch64_dsb_ish();

    aarch64_write_ttbr1_el1(new_ttbr1);
    aarch64_isb();
}

static void aarch64_sync_removed_mapping(virt_addr_t va)
{
    aarch64_dsb_ishst();
    aarch64_tlbi_vaael1is(va);
    aarch64_dsb_ish();
    aarch64_isb();
}

static bool aarch64_is_page_aligned(uint64_t value)
{
    return (value & AARCH64_PAGE_OFFSET_MASK) == 0;
}

static bool aarch64_is_pa_encodable(phys_addr_t pa)
{
    return (pa & ~AARCH64_PTE_ADDR_MASK) == 0;
}

static bool aarch64_is_canonical_va(virt_addr_t va)
{
    const uint64_t high_bits = va & AARCH64_CANONICAL_HIGH_MASK;
    return high_bits == 0 || high_bits == AARCH64_CANONICAL_HIGH_MASK;
}

static bool aarch64_is_canonical_range(virt_addr_t va_start, uint64_t size)
{
    const virt_addr_t last_va = va_start + size - PAGE_SIZE;
    if (!aarch64_is_canonical_va(va_start) || !aarch64_is_canonical_va(last_va))
    {
        return false;
    }

    return (va_start & AARCH64_CANONICAL_HIGH_MASK) == (last_va & AARCH64_CANONICAL_HIGH_MASK);
}

static uint64_t aarch64_chunk_size(virt_addr_t va, uint64_t remaining, uint64_t level_shift)
{
    const uint64_t span   = 1ULL << level_shift;
    const uint64_t offset = va & (span - 1ULL);
    const uint64_t chunk  = span - offset;
    return remaining < chunk ? remaining : chunk;
}

static uint64_t* aarch64_table_from_pa(phys_addr_t pa)
{
    virt_addr_t table_va = pa_to_hhdm((uintptr_t) pa, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);
    return (uint64_t*) table_va;
}

static bool aarch64_decode_table_entry(uint64_t entry, phys_addr_t* table_pa)
{
    if ((entry & AARCH64_PTE_VALID) == 0)
    {
        return false;
    }

    if ((entry & AARCH64_PTE_TABLE_OR_PAGE) == 0)
    {
        return false;
    }

    const phys_addr_t pa = entry & AARCH64_PTE_ADDR_MASK;
    if (!aarch64_is_page_aligned(pa))
    {
        return false;
    }

    *table_pa = pa;
    return true;
}

static bool aarch64_get_or_alloc_table(uint64_t* entry, phys_addr_t* table_pa)
{
    if ((*entry & AARCH64_PTE_VALID) != 0)
    {
        return aarch64_decode_table_entry(*entry, table_pa);
    }

    void* new_table = pmm_alloc(PAGE_SIZE);
    if (new_table == NULL)
    {
        kprintf("Arx kernel: arch_map_page failed: unable to allocate aarch64 table\n");
        return false;
    }

    memset(new_table, 0, PAGE_SIZE);
    const phys_addr_t new_table_pa = hhdm_to_pa((uintptr_t) new_table, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);

    *entry    = (new_table_pa & AARCH64_PTE_ADDR_MASK) | AARCH64_PTE_VALID | AARCH64_PTE_TABLE_OR_PAGE;
    *table_pa = new_table_pa;
    return true;
}

void __attribute__((weak)) arch_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, phys_addr_t page_table)
{
    if (!aarch64_is_canonical_va(va))
    {
        kprintf("Arx kernel: arch_map_page rejected non-canonical AArch64 VA: 0x%llx\n", (unsigned long long) va);
        return;
    }

    if (!aarch64_is_page_aligned(va) || !aarch64_is_page_aligned(pa) || !aarch64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_map_page rejected unaligned AArch64 VA/PA/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_map_page rejected null AArch64 page_table\n");
        return;
    }

    if (!aarch64_is_pa_encodable(pa) || !aarch64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_map_page rejected non-encodable AArch64 PA/page_table\n");
        return;
    }

    const uint64_t sanitized_flags = (flags & AARCH64_PTE_ALLOWED_MAP_FLAGS) | (1ULL << AARCH64_PTE_BIT_AF);

    const uint64_t l0_index = (va >> AARCH64_PT_SHIFT_L0) & AARCH64_PT_INDEX_MASK;
    const uint64_t l1_index = (va >> AARCH64_PT_SHIFT_L1) & AARCH64_PT_INDEX_MASK;
    const uint64_t l2_index = (va >> AARCH64_PT_SHIFT_L2) & AARCH64_PT_INDEX_MASK;
    const uint64_t l3_index = (va >> AARCH64_PT_SHIFT_L3) & AARCH64_PT_INDEX_MASK;

    uint64_t* l0 = aarch64_table_from_pa((uint64_t) page_table);

    phys_addr_t l1_pa = 0;
    if (!aarch64_get_or_alloc_table(&l0[l0_index], &l1_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at L0[%llu]\n", (unsigned long long) l0_index);
        return;
    }
    uint64_t* l1 = aarch64_table_from_pa(l1_pa);

    phys_addr_t l2_pa = 0;
    if (!aarch64_get_or_alloc_table(&l1[l1_index], &l2_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at L1[%llu]\n", (unsigned long long) l1_index);
        return;
    }
    uint64_t* l2 = aarch64_table_from_pa(l2_pa);

    phys_addr_t l3_pa = 0;
    if (!aarch64_get_or_alloc_table(&l2[l2_index], &l3_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at L2[%llu]\n", (unsigned long long) l2_index);
        return;
    }
    uint64_t* l3 = aarch64_table_from_pa(l3_pa);

    const uint64_t new_pte = (pa & AARCH64_PTE_ADDR_MASK) | sanitized_flags | AARCH64_PTE_VALID | AARCH64_PTE_TABLE_OR_PAGE;
    const uint64_t old_pte = l3[l3_index];

    if ((old_pte & AARCH64_PTE_VALID) != 0)
    {
        if ((old_pte & AARCH64_PTE_TABLE_OR_PAGE) == 0)
        {
            kprintf("Arx kernel: arch_map_page found malformed L3 descriptor at index %llu\n", (unsigned long long) l3_index);
            return;
        }

        if (old_pte == new_pte)
        {
            return;
        }

        l3[l3_index] = 0;
        aarch64_sync_removed_mapping(va);
    }

    l3[l3_index] = new_pte;
    aarch64_dsb_ishst();
    aarch64_isb();
}

void __attribute__((weak)) arch_unmap_page(virt_addr_t va, phys_addr_t page_table)
{
    if (!aarch64_is_canonical_va(va))
    {
        kprintf("Arx kernel: arch_unmap_page rejected non-canonical AArch64 VA: 0x%llx\n", (unsigned long long) va);
        return;
    }

    if (!aarch64_is_page_aligned(va) || !aarch64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_unmap_page rejected unaligned AArch64 VA/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_unmap_page rejected null AArch64 page_table\n");
        return;
    }

    if (!aarch64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_unmap_page rejected non-encodable AArch64 page_table\n");
        return;
    }

    const uint64_t l0_index = (va >> AARCH64_PT_SHIFT_L0) & AARCH64_PT_INDEX_MASK;
    const uint64_t l1_index = (va >> AARCH64_PT_SHIFT_L1) & AARCH64_PT_INDEX_MASK;
    const uint64_t l2_index = (va >> AARCH64_PT_SHIFT_L2) & AARCH64_PT_INDEX_MASK;
    const uint64_t l3_index = (va >> AARCH64_PT_SHIFT_L3) & AARCH64_PT_INDEX_MASK;

    uint64_t* l0 = aarch64_table_from_pa((uint64_t) page_table);

    phys_addr_t l1_pa = 0;
    if (!aarch64_decode_table_entry(l0[l0_index], &l1_pa))
    {
        return;
    }
    uint64_t* l1 = aarch64_table_from_pa(l1_pa);

    phys_addr_t l2_pa = 0;
    if (!aarch64_decode_table_entry(l1[l1_index], &l2_pa))
    {
        return;
    }
    uint64_t* l2 = aarch64_table_from_pa(l2_pa);

    phys_addr_t l3_pa = 0;
    if (!aarch64_decode_table_entry(l2[l2_index], &l3_pa))
    {
        return;
    }
    uint64_t* l3 = aarch64_table_from_pa(l3_pa);

    const uint64_t old_pte = l3[l3_index];
    if ((old_pte & AARCH64_PTE_VALID) == 0)
    {
        return;
    }

    if ((old_pte & AARCH64_PTE_TABLE_OR_PAGE) == 0)
    {
        kprintf("Arx kernel: arch_unmap_page found malformed L3 descriptor at index %llu\n", (unsigned long long) l3_index);
        return;
    }

    l3[l3_index] = 0;
    aarch64_sync_removed_mapping(va);
}

void __attribute__((weak)) arch_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, phys_addr_t page_table)
{
    if (size == 0)
    {
        return;
    }

    if (!aarch64_is_page_aligned(va_start) || !aarch64_is_page_aligned(pa_start) || !aarch64_is_page_aligned(size) || !aarch64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_map_range rejected unaligned AArch64 VA/PA/size/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_map_range rejected null AArch64 page_table\n");
        return;
    }

    if (va_start > UINT64_MAX - size || pa_start > UINT64_MAX - size)
    {
        kprintf("Arx kernel: arch_map_range rejected overflowing AArch64 VA/PA range\n");
        return;
    }

    if (!aarch64_is_canonical_range(va_start, size))
    {
        kprintf("Arx kernel: arch_map_range rejected non-canonical AArch64 VA range starting at 0x%llx\n", (unsigned long long) va_start);
        return;
    }

    const uint64_t    range_end = va_start + size;
    const phys_addr_t last_pa   = range_end == va_start ? pa_start : pa_start + size - PAGE_SIZE;
    if (!aarch64_is_pa_encodable(pa_start) || !aarch64_is_pa_encodable(last_pa) || !aarch64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_map_range rejected non-encodable AArch64 PA/page_table\n");
        return;
    }

    const uint64_t sanitized_flags = (flags & AARCH64_PTE_ALLOWED_MAP_FLAGS) | (1ULL << AARCH64_PTE_BIT_AF);
    const bool     active_pt       = page_table == arch_get_pt();

    bool        replacement_seen = false;
    bool        post_sync_phase  = false;
    bool        writes_pending   = false;
    uint64_t*   l0               = aarch64_table_from_pa((uint64_t) page_table);
    virt_addr_t va               = va_start;
    phys_addr_t pa               = pa_start;

    while (va < range_end)
    {
        const uint64_t l0_index = (va >> AARCH64_PT_SHIFT_L0) & AARCH64_PT_INDEX_MASK;
        const uint64_t l0_end   = va + aarch64_chunk_size(va, range_end - va, AARCH64_PT_SHIFT_L0);

        phys_addr_t l1_pa = 0;
        if (!aarch64_get_or_alloc_table(&l0[l0_index], &l1_pa))
        {
            kprintf("Arx kernel: arch_map_range failed at L0[%llu]\n", (unsigned long long) l0_index);
            goto out;
        }
        uint64_t* l1 = aarch64_table_from_pa(l1_pa);

        while (va < l0_end)
        {
            const uint64_t l1_index = (va >> AARCH64_PT_SHIFT_L1) & AARCH64_PT_INDEX_MASK;
            const uint64_t l1_end   = va + aarch64_chunk_size(va, l0_end - va, AARCH64_PT_SHIFT_L1);

            phys_addr_t l2_pa = 0;
            if (!aarch64_get_or_alloc_table(&l1[l1_index], &l2_pa))
            {
                kprintf("Arx kernel: arch_map_range failed at L1[%llu]\n", (unsigned long long) l1_index);
                goto out;
            }
            uint64_t* l2 = aarch64_table_from_pa(l2_pa);

            while (va < l1_end)
            {
                const uint64_t l2_index = (va >> AARCH64_PT_SHIFT_L2) & AARCH64_PT_INDEX_MASK;
                const uint64_t l2_end   = va + aarch64_chunk_size(va, l1_end - va, AARCH64_PT_SHIFT_L2);

                phys_addr_t l3_pa = 0;
                if (!aarch64_get_or_alloc_table(&l2[l2_index], &l3_pa))
                {
                    kprintf("Arx kernel: arch_map_range failed at L2[%llu]\n", (unsigned long long) l2_index);
                    goto out;
                }
                uint64_t*      l3          = aarch64_table_from_pa(l3_pa);
                const uint64_t l3_index    = (va >> AARCH64_PT_SHIFT_L3) & AARCH64_PT_INDEX_MASK;
                const uint64_t entry_count = (l2_end - va) >> PAGE_SHIFT;

                for (uint64_t entry = 0; entry < entry_count; ++entry)
                {
                    const phys_addr_t entry_pa = pa + (entry << PAGE_SHIFT);
                    const uint64_t    new_pte  = (entry_pa & AARCH64_PTE_ADDR_MASK) | sanitized_flags | AARCH64_PTE_VALID | AARCH64_PTE_TABLE_OR_PAGE;
                    const uint64_t    old_pte  = l3[l3_index + entry];

                    if ((old_pte & AARCH64_PTE_VALID) != 0 && (old_pte & AARCH64_PTE_TABLE_OR_PAGE) == 0)
                    {
                        kprintf("Arx kernel: arch_map_range found malformed L3 descriptor at index %llu\n", (unsigned long long) (l3_index + entry));
                        goto out;
                    }

                    if (old_pte == new_pte)
                    {
                        continue;
                    }

                    if (active_pt && (old_pte & AARCH64_PTE_VALID) != 0)
                    {
                        l3[l3_index + entry] = 0;
                        replacement_seen     = true;
                        writes_pending       = true;
                    }
                    else
                    {
                        l3[l3_index + entry] = new_pte;
                        writes_pending       = true;
                    }
                }

                pa += l2_end - va;
                va = l2_end;
            }
        }
    }

    if (active_pt && replacement_seen)
    {
        aarch64_sync_current_page_table();
        post_sync_phase = true;
        writes_pending  = false;

        va = va_start;
        pa = pa_start;
        while (va < range_end)
        {
            const uint64_t l0_index = (va >> AARCH64_PT_SHIFT_L0) & AARCH64_PT_INDEX_MASK;
            const uint64_t l0_end   = va + aarch64_chunk_size(va, range_end - va, AARCH64_PT_SHIFT_L0);

            phys_addr_t l1_pa = 0;
            if (!aarch64_get_or_alloc_table(&l0[l0_index], &l1_pa))
            {
                kprintf("Arx kernel: arch_map_range failed at L0[%llu] during remap\n", (unsigned long long) l0_index);
                goto out;
            }
            uint64_t* l1 = aarch64_table_from_pa(l1_pa);

            while (va < l0_end)
            {
                const uint64_t l1_index = (va >> AARCH64_PT_SHIFT_L1) & AARCH64_PT_INDEX_MASK;
                const uint64_t l1_end   = va + aarch64_chunk_size(va, l0_end - va, AARCH64_PT_SHIFT_L1);

                phys_addr_t l2_pa = 0;
                if (!aarch64_get_or_alloc_table(&l1[l1_index], &l2_pa))
                {
                    kprintf("Arx kernel: arch_map_range failed at L1[%llu] during remap\n", (unsigned long long) l1_index);
                    goto out;
                }
                uint64_t* l2 = aarch64_table_from_pa(l2_pa);

                while (va < l1_end)
                {
                    const uint64_t l2_index = (va >> AARCH64_PT_SHIFT_L2) & AARCH64_PT_INDEX_MASK;
                    const uint64_t l2_end   = va + aarch64_chunk_size(va, l1_end - va, AARCH64_PT_SHIFT_L2);

                    phys_addr_t l3_pa = 0;
                    if (!aarch64_get_or_alloc_table(&l2[l2_index], &l3_pa))
                    {
                        kprintf("Arx kernel: arch_map_range failed at L2[%llu] during remap\n", (unsigned long long) l2_index);
                        goto out;
                    }
                    uint64_t*      l3          = aarch64_table_from_pa(l3_pa);
                    const uint64_t l3_index    = (va >> AARCH64_PT_SHIFT_L3) & AARCH64_PT_INDEX_MASK;
                    const uint64_t entry_count = (l2_end - va) >> PAGE_SHIFT;

                    for (uint64_t entry = 0; entry < entry_count; ++entry)
                    {
                        const phys_addr_t entry_pa = pa + (entry << PAGE_SHIFT);
                        const uint64_t    new_pte  = (entry_pa & AARCH64_PTE_ADDR_MASK) | sanitized_flags | AARCH64_PTE_VALID | AARCH64_PTE_TABLE_OR_PAGE;
                        if (l3[l3_index + entry] == new_pte)
                        {
                            continue;
                        }

                        l3[l3_index + entry] = new_pte;
                        writes_pending       = true;
                    }

                    pa += l2_end - va;
                    va = l2_end;
                }
            }
        }
    }

out:
    if (active_pt && replacement_seen && !post_sync_phase)
    {
        aarch64_sync_current_page_table();
        writes_pending = false;
    }

    if (active_pt && writes_pending)
    {
        aarch64_dsb_ishst();
        aarch64_isb();
    }
}

void __attribute__((weak)) arch_unmap_range(virt_addr_t va_start, uint64_t size, phys_addr_t page_table)
{
    if (size == 0)
    {
        return;
    }

    if (!aarch64_is_page_aligned(va_start) || !aarch64_is_page_aligned(size) || !aarch64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_unmap_range rejected unaligned AArch64 VA/size/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_unmap_range rejected null AArch64 page_table\n");
        return;
    }

    if (va_start > UINT64_MAX - size)
    {
        kprintf("Arx kernel: arch_unmap_range rejected overflowing AArch64 VA range\n");
        return;
    }

    if (!aarch64_is_canonical_range(va_start, size))
    {
        kprintf("Arx kernel: arch_unmap_range rejected non-canonical AArch64 VA range starting at 0x%llx\n", (unsigned long long) va_start);
        return;
    }

    if (!aarch64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_unmap_range rejected non-encodable AArch64 page_table\n");
        return;
    }

    const bool     active_pt      = page_table == arch_get_pt();
    const uint64_t range_end      = va_start + size;
    bool           writes_pending = false;
    uint64_t*      l0             = aarch64_table_from_pa((uint64_t) page_table);
    virt_addr_t    va             = va_start;

    while (va < range_end)
    {
        const uint64_t l0_index = (va >> AARCH64_PT_SHIFT_L0) & AARCH64_PT_INDEX_MASK;
        const uint64_t l0_end   = va + aarch64_chunk_size(va, range_end - va, AARCH64_PT_SHIFT_L0);

        phys_addr_t l1_pa = 0;
        if (!aarch64_decode_table_entry(l0[l0_index], &l1_pa))
        {
            va = l0_end;
            continue;
        }
        uint64_t* l1 = aarch64_table_from_pa(l1_pa);

        while (va < l0_end)
        {
            const uint64_t l1_index = (va >> AARCH64_PT_SHIFT_L1) & AARCH64_PT_INDEX_MASK;
            const uint64_t l1_end   = va + aarch64_chunk_size(va, l0_end - va, AARCH64_PT_SHIFT_L1);

            phys_addr_t l2_pa = 0;
            if (!aarch64_decode_table_entry(l1[l1_index], &l2_pa))
            {
                va = l1_end;
                continue;
            }
            uint64_t* l2 = aarch64_table_from_pa(l2_pa);

            while (va < l1_end)
            {
                const uint64_t l2_index = (va >> AARCH64_PT_SHIFT_L2) & AARCH64_PT_INDEX_MASK;
                const uint64_t l2_end   = va + aarch64_chunk_size(va, l1_end - va, AARCH64_PT_SHIFT_L2);

                phys_addr_t l3_pa = 0;
                if (!aarch64_decode_table_entry(l2[l2_index], &l3_pa))
                {
                    va = l2_end;
                    continue;
                }
                uint64_t*      l3          = aarch64_table_from_pa(l3_pa);
                const uint64_t l3_index    = (va >> AARCH64_PT_SHIFT_L3) & AARCH64_PT_INDEX_MASK;
                const uint64_t entry_count = (l2_end - va) >> PAGE_SHIFT;

                for (uint64_t entry = 0; entry < entry_count; ++entry)
                {
                    const uint64_t old_pte = l3[l3_index + entry];
                    if ((old_pte & AARCH64_PTE_VALID) == 0)
                    {
                        continue;
                    }

                    if ((old_pte & AARCH64_PTE_TABLE_OR_PAGE) == 0)
                    {
                        kprintf("Arx kernel: arch_unmap_range found malformed L3 descriptor at index %llu\n", (unsigned long long) (l3_index + entry));
                        goto out_unmap;
                    }

                    l3[l3_index + entry] = 0;
                    writes_pending       = true;
                }

                va = l2_end;
            }
        }
    }

out_unmap:
    if (active_pt && writes_pending)
    {
        aarch64_sync_current_page_table();
    }
}

void __attribute__((weak)) arch_protect(virt_addr_t va, uint64_t flags, phys_addr_t page_table)
{
    if (!aarch64_is_canonical_va(va))
    {
        kprintf("Arx kernel: arch_protect rejected non-canonical AArch64 VA: 0x%llx\n", (unsigned long long) va);
        return;
    }

    if (!aarch64_is_page_aligned(va) || !aarch64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_protect rejected unaligned AArch64 VA/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_protect rejected null AArch64 page_table\n");
        return;
    }

    if (!aarch64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_protect rejected non-encodable AArch64 page_table\n");
        return;
    }

    const uint64_t sanitized_flags = (flags & AARCH64_PTE_ALLOWED_MAP_FLAGS) | (1ULL << AARCH64_PTE_BIT_AF);

    const uint64_t l0_index = (va >> AARCH64_PT_SHIFT_L0) & AARCH64_PT_INDEX_MASK;
    const uint64_t l1_index = (va >> AARCH64_PT_SHIFT_L1) & AARCH64_PT_INDEX_MASK;
    const uint64_t l2_index = (va >> AARCH64_PT_SHIFT_L2) & AARCH64_PT_INDEX_MASK;
    const uint64_t l3_index = (va >> AARCH64_PT_SHIFT_L3) & AARCH64_PT_INDEX_MASK;

    uint64_t* l0 = aarch64_table_from_pa((uint64_t) page_table);

    phys_addr_t l1_pa = 0;
    if (!aarch64_decode_table_entry(l0[l0_index], &l1_pa))
    {
        return;
    }
    uint64_t* l1 = aarch64_table_from_pa(l1_pa);

    phys_addr_t l2_pa = 0;
    if (!aarch64_decode_table_entry(l1[l1_index], &l2_pa))
    {
        return;
    }
    uint64_t* l2 = aarch64_table_from_pa(l2_pa);

    phys_addr_t l3_pa = 0;
    if (!aarch64_decode_table_entry(l2[l2_index], &l3_pa))
    {
        return;
    }
    uint64_t* l3 = aarch64_table_from_pa(l3_pa);

    const uint64_t old_pte = l3[l3_index];
    if ((old_pte & AARCH64_PTE_VALID) == 0)
    {
        return;
    }

    if ((old_pte & AARCH64_PTE_TABLE_OR_PAGE) == 0)
    {
        kprintf("Arx kernel: arch_protect found malformed L3 descriptor at index %llu\n", (unsigned long long) l3_index);
        return;
    }

    const uint64_t new_pte = (old_pte & AARCH64_PTE_ADDR_MASK) | sanitized_flags | AARCH64_PTE_VALID | AARCH64_PTE_TABLE_OR_PAGE;
    if (old_pte == new_pte)
    {
        return;
    }

    l3[l3_index] = new_pte;
    if (page_table == arch_get_pt())
    {
        aarch64_sync_removed_mapping(va);
    }
}

void __attribute__((weak)) arch_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, phys_addr_t page_table)
{
    if (size == 0)
    {
        return;
    }

    if (!aarch64_is_page_aligned(va_start) || !aarch64_is_page_aligned(size) || !aarch64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_protect_range rejected unaligned AArch64 VA/size/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_protect_range rejected null AArch64 page_table\n");
        return;
    }

    if (va_start > UINT64_MAX - size)
    {
        kprintf("Arx kernel: arch_protect_range rejected overflowing AArch64 VA range\n");
        return;
    }

    if (!aarch64_is_canonical_range(va_start, size))
    {
        kprintf("Arx kernel: arch_protect_range rejected non-canonical AArch64 VA range starting at 0x%llx\n", (unsigned long long) va_start);
        return;
    }

    if (!aarch64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_protect_range rejected non-encodable AArch64 page_table\n");
        return;
    }

    const uint64_t sanitized_flags = (flags & AARCH64_PTE_ALLOWED_MAP_FLAGS) | (1ULL << AARCH64_PTE_BIT_AF);
    const bool     active_pt       = page_table == arch_get_pt();
    const uint64_t range_end       = va_start + size;

    bool        writes_pending = false;
    uint64_t*   l0             = aarch64_table_from_pa((uint64_t) page_table);
    virt_addr_t va             = va_start;

    while (va < range_end)
    {
        const uint64_t l0_index = (va >> AARCH64_PT_SHIFT_L0) & AARCH64_PT_INDEX_MASK;
        const uint64_t l0_end   = va + aarch64_chunk_size(va, range_end - va, AARCH64_PT_SHIFT_L0);

        phys_addr_t l1_pa = 0;
        if (!aarch64_decode_table_entry(l0[l0_index], &l1_pa))
        {
            va = l0_end;
            continue;
        }
        uint64_t* l1 = aarch64_table_from_pa(l1_pa);

        while (va < l0_end)
        {
            const uint64_t l1_index = (va >> AARCH64_PT_SHIFT_L1) & AARCH64_PT_INDEX_MASK;
            const uint64_t l1_end   = va + aarch64_chunk_size(va, l0_end - va, AARCH64_PT_SHIFT_L1);

            phys_addr_t l2_pa = 0;
            if (!aarch64_decode_table_entry(l1[l1_index], &l2_pa))
            {
                va = l1_end;
                continue;
            }
            uint64_t* l2 = aarch64_table_from_pa(l2_pa);

            while (va < l1_end)
            {
                const uint64_t l2_index = (va >> AARCH64_PT_SHIFT_L2) & AARCH64_PT_INDEX_MASK;
                const uint64_t l2_end   = va + aarch64_chunk_size(va, l1_end - va, AARCH64_PT_SHIFT_L2);

                phys_addr_t l3_pa = 0;
                if (!aarch64_decode_table_entry(l2[l2_index], &l3_pa))
                {
                    va = l2_end;
                    continue;
                }
                uint64_t*      l3          = aarch64_table_from_pa(l3_pa);
                const uint64_t l3_index    = (va >> AARCH64_PT_SHIFT_L3) & AARCH64_PT_INDEX_MASK;
                const uint64_t entry_count = (l2_end - va) >> PAGE_SHIFT;

                for (uint64_t entry = 0; entry < entry_count; ++entry)
                {
                    const uint64_t old_pte = l3[l3_index + entry];
                    if ((old_pte & AARCH64_PTE_VALID) == 0)
                    {
                        continue;
                    }

                    if ((old_pte & AARCH64_PTE_TABLE_OR_PAGE) == 0)
                    {
                        kprintf("Arx kernel: arch_protect_range found malformed L3 descriptor at index %llu\n", (unsigned long long) (l3_index + entry));
                        goto out_protect;
                    }

                    const uint64_t new_pte = (old_pte & AARCH64_PTE_ADDR_MASK) | sanitized_flags | AARCH64_PTE_VALID | AARCH64_PTE_TABLE_OR_PAGE;
                    if (old_pte == new_pte)
                    {
                        continue;
                    }

                    l3[l3_index + entry] = new_pte;
                    writes_pending       = true;
                }

                va = l2_end;
            }
        }
    }

out_protect:
    if (active_pt && writes_pending)
    {
        aarch64_sync_current_page_table();
    }
}

phys_addr_t __attribute__((weak)) arch_virt_to_phys(virt_addr_t va, phys_addr_t page_table)
{
    if (!aarch64_is_canonical_va(va))
    {
        return 0;
    }

    if (!aarch64_is_page_aligned((uint64_t) page_table) || page_table == 0 || !aarch64_is_pa_encodable((uint64_t) page_table))
    {
        return 0;
    }

    const uint64_t l0_index = (va >> AARCH64_PT_SHIFT_L0) & AARCH64_PT_INDEX_MASK;
    const uint64_t l1_index = (va >> AARCH64_PT_SHIFT_L1) & AARCH64_PT_INDEX_MASK;
    const uint64_t l2_index = (va >> AARCH64_PT_SHIFT_L2) & AARCH64_PT_INDEX_MASK;
    const uint64_t l3_index = (va >> AARCH64_PT_SHIFT_L3) & AARCH64_PT_INDEX_MASK;

    uint64_t* l0 = aarch64_table_from_pa((uint64_t) page_table);

    phys_addr_t l1_pa = 0;
    if (!aarch64_decode_table_entry(l0[l0_index], &l1_pa))
    {
        return 0;
    }
    uint64_t* l1 = aarch64_table_from_pa(l1_pa);

    phys_addr_t l2_pa = 0;
    if (!aarch64_decode_table_entry(l1[l1_index], &l2_pa))
    {
        return 0;
    }
    uint64_t* l2 = aarch64_table_from_pa(l2_pa);

    phys_addr_t l3_pa = 0;
    if (!aarch64_decode_table_entry(l2[l2_index], &l3_pa))
    {
        return 0;
    }
    uint64_t* l3 = aarch64_table_from_pa(l3_pa);

    const uint64_t pte = l3[l3_index];
    if ((pte & AARCH64_PTE_VALID) == 0 || (pte & AARCH64_PTE_TABLE_OR_PAGE) == 0)
    {
        return 0;
    }

    return (pte & AARCH64_PTE_ADDR_MASK) | (va & AARCH64_PAGE_OFFSET_MASK);
}
