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

#define X86_64_PTE_PRESENT (1ULL << X86_64_PTE_BIT_PRESENT)
#define X86_64_PTE_WRITABLE (1ULL << X86_64_PTE_BIT_WRITABLE)
#define X86_64_PTE_USER (1ULL << X86_64_PTE_BIT_USER)

#define X86_64_PHYS_ADDR_BITS 52ULL
#define X86_64_PAGE_OFFSET_MASK ((1ULL << PAGE_SHIFT) - 1ULL)
#define X86_64_PHYS_ADDR_MASK ((1ULL << X86_64_PHYS_ADDR_BITS) - 1ULL)
#define X86_64_PTE_ADDR_MASK (X86_64_PHYS_ADDR_MASK & ~X86_64_PAGE_OFFSET_MASK)

static uint64_t* x86_64_table_from_pa(uint64_t pa)
{
    uintptr_t table_va = pa_to_hhdm((uintptr_t) pa, pmm_zone.hhdm_present, pmm_zone.hhdm_offset);
    return (uint64_t*) table_va;
}

static uint64_t x86_64_get_or_alloc_table(uint64_t* entry, uint64_t inherited_flags)
{
    if ((*entry & X86_64_PTE_PRESENT) != 0)
    {
        return *entry & X86_64_PTE_ADDR_MASK;
    }

    void* new_table = pmm_alloc(&pmm_zone, PAGE_SIZE);
    if (new_table == NULL)
    {
        panic();
    }

    memset(new_table, 0, PAGE_SIZE);
    const uint64_t new_table_pa = hhdm_to_pa((uintptr_t) new_table, pmm_zone.hhdm_present, pmm_zone.hhdm_offset);
    const uint64_t table_flags  = X86_64_PTE_PRESENT | X86_64_PTE_WRITABLE | (inherited_flags & X86_64_PTE_USER);

    *entry = (new_table_pa & X86_64_PTE_ADDR_MASK) | table_flags;
    return new_table_pa;
}

void __attribute__((weak)) arch_map_page(uint64_t va, uint64_t pa, uint64_t flags, uintptr_t page_table)
{
    const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
    const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
    const uint64_t pd_index   = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
    const uint64_t pt_index   = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;

    uint64_t* pml4 = x86_64_table_from_pa((uint64_t) page_table);
    
    uint64_t  pdpt_pa = x86_64_get_or_alloc_table(&pml4[pml4_index], flags);
    uint64_t* pdpt    = x86_64_table_from_pa(pdpt_pa);

    uint64_t  pd_pa = x86_64_get_or_alloc_table(&pdpt[pdpt_index], flags);
    uint64_t* pd    = x86_64_table_from_pa(pd_pa);

    uint64_t  pt_pa = x86_64_get_or_alloc_table(&pd[pd_index], flags);
    uint64_t* pt    = x86_64_table_from_pa(pt_pa);

    pt[pt_index] = (pa & X86_64_PTE_ADDR_MASK) | flags | X86_64_PTE_PRESENT;
}

void __attribute__((weak)) arch_unmap_page(uint64_t va, uintptr_t page_table)
{
    const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
    const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
    const uint64_t pd_index   = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
    const uint64_t pt_index   = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;

    uint64_t* pml4 = x86_64_table_from_pa((uint64_t) page_table);
    if ((pml4[pml4_index] & X86_64_PTE_PRESENT) == 0)
    {
        return;
    }

    uint64_t* pdpt = x86_64_table_from_pa(pml4[pml4_index] & X86_64_PTE_ADDR_MASK);
    if ((pdpt[pdpt_index] & X86_64_PTE_PRESENT) == 0)
    {
        return;
    }

    uint64_t* pd = x86_64_table_from_pa(pdpt[pdpt_index] & X86_64_PTE_ADDR_MASK);
    if ((pd[pd_index] & X86_64_PTE_PRESENT) == 0)
    {
        return;
    }

    uint64_t* pt = x86_64_table_from_pa(pd[pd_index] & X86_64_PTE_ADDR_MASK);
    pt[pt_index] = 0;
}
