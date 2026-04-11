#include <arch/arch.h>
#include <memory/pmm.h>

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

#define AARCH64_PTE_ALLOWED_MAP_FLAGS                                                                                                      \
    ((1ULL << AARCH64_PTE_BIT_ATTRINDX_0) | (1ULL << AARCH64_PTE_BIT_ATTRINDX_1) | (1ULL << AARCH64_PTE_BIT_ATTRINDX_2) |                \
     (1ULL << AARCH64_PTE_BIT_NS) | (1ULL << AARCH64_PTE_BIT_AP_0) | (1ULL << AARCH64_PTE_BIT_AP_1) | (1ULL << AARCH64_PTE_BIT_SH_0) |   \
     (1ULL << AARCH64_PTE_BIT_SH_1) | (1ULL << AARCH64_PTE_BIT_AF) | (1ULL << AARCH64_PTE_BIT_nG) | (1ULL << AARCH64_PTE_BIT_DBM) |       \
     (1ULL << AARCH64_PTE_BIT_CONTIGUOUS) | (1ULL << AARCH64_PTE_BIT_PXN) | (1ULL << AARCH64_PTE_BIT_UXN))

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

static inline void aarch64_tlbi_vaael1is(uint64_t va)
{
    const uint64_t tlbi_operand = va >> PAGE_SHIFT;
    __asm__ volatile("tlbi vaae1is, %0" : : "r"(tlbi_operand) : "memory");
}

static void aarch64_sync_removed_mapping(uint64_t va)
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

static bool aarch64_is_pa_encodable(uint64_t pa)
{
    return (pa & ~AARCH64_PTE_ADDR_MASK) == 0;
}

static bool aarch64_is_canonical_va(uint64_t va)
{
    const uint64_t high_bits = va & AARCH64_CANONICAL_HIGH_MASK;
    return high_bits == 0 || high_bits == AARCH64_CANONICAL_HIGH_MASK;
}

static uint64_t* aarch64_table_from_pa(uint64_t pa)
{
    uintptr_t table_va = pa_to_hhdm((uintptr_t) pa, pmm_zone.hhdm_present, pmm_zone.hhdm_offset);
    return (uint64_t*) table_va;
}

static bool aarch64_decode_table_entry(uint64_t entry, uint64_t* table_pa)
{
    if ((entry & AARCH64_PTE_VALID) == 0)
    {
        return false;
    }

    if ((entry & AARCH64_PTE_TABLE_OR_PAGE) == 0)
    {
        return false;
    }

    const uint64_t pa = entry & AARCH64_PTE_ADDR_MASK;
    if (!aarch64_is_page_aligned(pa))
    {
        return false;
    }

    *table_pa = pa;
    return true;
}

static bool aarch64_get_or_alloc_table(uint64_t* entry, uint64_t* table_pa)
{
    if ((*entry & AARCH64_PTE_VALID) != 0)
    {
        return aarch64_decode_table_entry(*entry, table_pa);
    }

    void* new_table = pmm_alloc(&pmm_zone, PAGE_SIZE);
    if (new_table == NULL)
    {
        kprintf("Arx kernel: arch_map_page failed: unable to allocate aarch64 table\n");
        return false;
    }

    memset(new_table, 0, PAGE_SIZE);
    const uint64_t new_table_pa = hhdm_to_pa((uintptr_t) new_table, pmm_zone.hhdm_present, pmm_zone.hhdm_offset);

    *entry    = (new_table_pa & AARCH64_PTE_ADDR_MASK) | AARCH64_PTE_VALID | AARCH64_PTE_TABLE_OR_PAGE;
    *table_pa = new_table_pa;
    return true;
}

void __attribute__((weak)) arch_map_page(uint64_t va, uint64_t pa, uint64_t flags, uintptr_t page_table)
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

    uint64_t l1_pa = 0;
    if (!aarch64_get_or_alloc_table(&l0[l0_index], &l1_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at L0[%llu]\n", (unsigned long long) l0_index);
        return;
    }
    uint64_t* l1 = aarch64_table_from_pa(l1_pa);

    uint64_t l2_pa = 0;
    if (!aarch64_get_or_alloc_table(&l1[l1_index], &l2_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at L1[%llu]\n", (unsigned long long) l1_index);
        return;
    }
    uint64_t* l2 = aarch64_table_from_pa(l2_pa);

    uint64_t l3_pa = 0;
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

void __attribute__((weak)) arch_unmap_page(uint64_t va, uintptr_t page_table)
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

    uint64_t l1_pa = 0;
    if (!aarch64_decode_table_entry(l0[l0_index], &l1_pa))
    {
        return;
    }
    uint64_t* l1 = aarch64_table_from_pa(l1_pa);

    uint64_t l2_pa = 0;
    if (!aarch64_decode_table_entry(l1[l1_index], &l2_pa))
    {
        return;
    }
    uint64_t* l2 = aarch64_table_from_pa(l2_pa);

    uint64_t l3_pa = 0;
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
