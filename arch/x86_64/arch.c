#include <arch/arch.h>
#include <memory/pmm.h>

#define X86_64_PT_LEVEL_BITS 9ULL
#define X86_64_PT_ENTRIES (1ULL << X86_64_PT_LEVEL_BITS)
#define X86_64_PT_INDEX_MASK (X86_64_PT_ENTRIES - 1ULL)

#define X86_64_PT_SHIFT_PT PAGE_SHIFT
#define X86_64_PT_SHIFT_PD (X86_64_PT_SHIFT_PT + X86_64_PT_LEVEL_BITS)
#define X86_64_PT_SHIFT_PDPT (X86_64_PT_SHIFT_PD + X86_64_PT_LEVEL_BITS)
#define X86_64_PT_SHIFT_PML4 (X86_64_PT_SHIFT_PDPT + X86_64_PT_LEVEL_BITS)

#define X86_64_PTE_BIT_PRESENT 0ULL
#define X86_64_PTE_BIT_WRITABLE 1ULL
#define X86_64_PTE_BIT_USER 2ULL
#define X86_64_PTE_BIT_PAGE_WRITE_THROUGH 3ULL
#define X86_64_PTE_BIT_PAGE_CACHE_DISABLE 4ULL
#define X86_64_PTE_BIT_ACCESSED 5ULL
#define X86_64_PTE_BIT_DIRTY 6ULL
#define X86_64_PTE_BIT_PAGE_SIZE_OR_PAT 7ULL
#define X86_64_PTE_BIT_GLOBAL 8ULL
#define X86_64_PTE_BIT_NO_EXECUTE 63ULL

#define X86_64_PTE_PRESENT (1ULL << X86_64_PTE_BIT_PRESENT)
#define X86_64_PTE_WRITABLE (1ULL << X86_64_PTE_BIT_WRITABLE)
#define X86_64_PTE_USER (1ULL << X86_64_PTE_BIT_USER)
#define X86_64_PTE_PAGE_SIZE_OR_PAT (1ULL << X86_64_PTE_BIT_PAGE_SIZE_OR_PAT)
#define X86_64_PTE_NO_EXECUTE (1ULL << X86_64_PTE_BIT_NO_EXECUTE)

#define X86_64_PHYS_ADDR_BITS 52ULL
#define X86_64_PAGE_OFFSET_MASK ((1ULL << PAGE_SHIFT) - 1ULL)
#define X86_64_PHYS_ADDR_MASK ((1ULL << X86_64_PHYS_ADDR_BITS) - 1ULL)
#define X86_64_PTE_ADDR_MASK (X86_64_PHYS_ADDR_MASK & ~X86_64_PAGE_OFFSET_MASK)

#define X86_64_CANONICAL_VA_BITS 48ULL
#define X86_64_CANONICAL_LOW_MASK ((1ULL << X86_64_CANONICAL_VA_BITS) - 1ULL)
#define X86_64_CANONICAL_HIGH_MASK (~X86_64_CANONICAL_LOW_MASK)

#define X86_64_PTE_ALLOWED_MAP_FLAGS                                                                                                                                        \
    ((1ULL << X86_64_PTE_BIT_WRITABLE) | (1ULL << X86_64_PTE_BIT_USER) | (1ULL << X86_64_PTE_BIT_PAGE_WRITE_THROUGH) | (1ULL << X86_64_PTE_BIT_PAGE_CACHE_DISABLE) | \
     (1ULL << X86_64_PTE_BIT_ACCESSED) | (1ULL << X86_64_PTE_BIT_DIRTY) | (1ULL << X86_64_PTE_BIT_PAGE_SIZE_OR_PAT) | (1ULL << X86_64_PTE_BIT_GLOBAL) | X86_64_PTE_NO_EXECUTE)

static bool x86_64_is_page_aligned(uint64_t value)
{
    return (value & X86_64_PAGE_OFFSET_MASK) == 0;
}

static bool x86_64_is_canonical_va(uint64_t va)
{
    const uint64_t high_bits = va & X86_64_CANONICAL_HIGH_MASK;
    return high_bits == 0 || high_bits == X86_64_CANONICAL_HIGH_MASK;
}

static uint64_t* x86_64_table_from_pa(uint64_t pa)
{
    uintptr_t table_va = pa_to_hhdm((uintptr_t) pa, pmm_zone.hhdm_present, pmm_zone.hhdm_offset);
    return (uint64_t*) table_va;
}

static bool x86_64_decode_table_entry(uint64_t entry, uint64_t* table_pa)
{
    if ((entry & X86_64_PTE_PRESENT) == 0)
    {
        return false;
    }

    if ((entry & X86_64_PTE_PAGE_SIZE_OR_PAT) != 0)
    {
        return false;
    }

    const uint64_t pa = entry & X86_64_PTE_ADDR_MASK;
    if (!x86_64_is_page_aligned(pa))
    {
        return false;
    }

    *table_pa = pa;
    return true;
}

static bool x86_64_get_or_alloc_table(uint64_t* entry, uint64_t inherited_flags, uint64_t* table_pa)
{
    if ((*entry & X86_64_PTE_PRESENT) != 0)
    {
        return x86_64_decode_table_entry(*entry, table_pa);
    }

    void* new_table = pmm_alloc(&pmm_zone, PAGE_SIZE);
    if (new_table == NULL)
    {
        kprintf("Arx kernel: arch_map_page failed: unable to allocate page table\n");
        return false;
    }

    memset(new_table, 0, PAGE_SIZE);
    const uint64_t new_table_pa = hhdm_to_pa((uintptr_t) new_table, pmm_zone.hhdm_present, pmm_zone.hhdm_offset);
    const uint64_t table_flags  = X86_64_PTE_PRESENT | X86_64_PTE_WRITABLE | (inherited_flags & X86_64_PTE_USER);

    *entry = (new_table_pa & X86_64_PTE_ADDR_MASK) | table_flags;
    *table_pa = new_table_pa;
    return true;
}

void __attribute__((weak)) arch_map_page(uint64_t va, uint64_t pa, uint64_t flags, uintptr_t page_table)
{
    if (!x86_64_is_canonical_va(va))
    {
        kprintf("Arx kernel: arch_map_page rejected non-canonical VA: 0x%llx\n", (unsigned long long) va);
        return;
    }

    if (!x86_64_is_page_aligned(va) || !x86_64_is_page_aligned(pa) || !x86_64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_map_page rejected unaligned VA/PA/page_table\n");
        return;
    }

    if ((page_table & X86_64_PTE_ADDR_MASK) == 0)
    {
        kprintf("Arx kernel: arch_map_page rejected null page_table\n");
        return;
    }

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;

    const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
    const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
    const uint64_t pd_index   = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
    const uint64_t pt_index   = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;

    uint64_t* pml4 = x86_64_table_from_pa((uint64_t) page_table);

    uint64_t pdpt_pa = 0;
    if (!x86_64_get_or_alloc_table(&pml4[pml4_index], sanitized_flags, &pdpt_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at PML4[%llu]\n", (unsigned long long) pml4_index);
        return;
    }
    uint64_t* pdpt    = x86_64_table_from_pa(pdpt_pa);

    uint64_t pd_pa = 0;
    if (!x86_64_get_or_alloc_table(&pdpt[pdpt_index], sanitized_flags, &pd_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at PDPT[%llu]\n", (unsigned long long) pdpt_index);
        return;
    }
    uint64_t* pd    = x86_64_table_from_pa(pd_pa);

    uint64_t pt_pa = 0;
    if (!x86_64_get_or_alloc_table(&pd[pd_index], sanitized_flags, &pt_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at PD[%llu]\n", (unsigned long long) pd_index);
        return;
    }
    uint64_t* pt    = x86_64_table_from_pa(pt_pa);

    pt[pt_index] = (pa & X86_64_PTE_ADDR_MASK) | sanitized_flags | X86_64_PTE_PRESENT;
}

void __attribute__((weak)) arch_unmap_page(uint64_t va, uintptr_t page_table)
{
    if (!x86_64_is_canonical_va(va))
    {
        kprintf("Arx kernel: arch_unmap_page rejected non-canonical VA: 0x%llx\n", (unsigned long long) va);
        return;
    }

    if (!x86_64_is_page_aligned(va) || !x86_64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_unmap_page rejected unaligned VA/page_table\n");
        return;
    }

    if ((page_table & X86_64_PTE_ADDR_MASK) == 0)
    {
        kprintf("Arx kernel: arch_unmap_page rejected null page_table\n");
        return;
    }

    const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
    const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
    const uint64_t pd_index   = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
    const uint64_t pt_index   = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;

    uint64_t* pml4 = x86_64_table_from_pa((uint64_t) page_table);

    uint64_t pdpt_pa = 0;
    if (!x86_64_decode_table_entry(pml4[pml4_index], &pdpt_pa))
    {
        return;
    }

    uint64_t* pdpt = x86_64_table_from_pa(pdpt_pa);

    uint64_t pd_pa = 0;
    if (!x86_64_decode_table_entry(pdpt[pdpt_index], &pd_pa))
    {
        return;
    }

    uint64_t* pd = x86_64_table_from_pa(pd_pa);

    uint64_t pt_pa = 0;
    if (!x86_64_decode_table_entry(pd[pd_index], &pt_pa))
    {
        return;
    }

    uint64_t* pt = x86_64_table_from_pa(pt_pa);
    pt[pt_index] = 0;
}
