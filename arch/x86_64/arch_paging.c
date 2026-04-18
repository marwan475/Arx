#include <arch/arch.h>
#include <memory/pmm.h>
#include <memory/vmm.h>

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
#define X86_64_PTE_GLOBAL (1ULL << X86_64_PTE_BIT_GLOBAL)
#define X86_64_PTE_NO_EXECUTE (1ULL << X86_64_PTE_BIT_NO_EXECUTE)

#define X86_64_PHYS_ADDR_BITS 52ULL
#define X86_64_PAGE_OFFSET_MASK ((1ULL << PAGE_SHIFT) - 1ULL)
#define X86_64_PHYS_ADDR_MASK ((1ULL << X86_64_PHYS_ADDR_BITS) - 1ULL)
#define X86_64_PTE_ADDR_MASK (X86_64_PHYS_ADDR_MASK & ~X86_64_PAGE_OFFSET_MASK)

#define X86_64_CANONICAL_VA_BITS 48ULL
#define X86_64_CANONICAL_LOW_MASK ((1ULL << X86_64_CANONICAL_VA_BITS) - 1ULL)
#define X86_64_CANONICAL_HIGH_MASK (~X86_64_CANONICAL_LOW_MASK)

#define X86_64_PTE_ALLOWED_MAP_FLAGS                                                                                                                                                                                                                                                                       \
    ((1ULL << X86_64_PTE_BIT_WRITABLE) | (1ULL << X86_64_PTE_BIT_USER) | (1ULL << X86_64_PTE_BIT_PAGE_WRITE_THROUGH) | (1ULL << X86_64_PTE_BIT_PAGE_CACHE_DISABLE) | (1ULL << X86_64_PTE_BIT_ACCESSED) | (1ULL << X86_64_PTE_BIT_DIRTY) | (1ULL << X86_64_PTE_BIT_PAGE_SIZE_OR_PAT)                        \
     | (1ULL << X86_64_PTE_BIT_GLOBAL) | X86_64_PTE_NO_EXECUTE)

static inline void x86_64_invlpg(virt_addr_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static inline void x86_64_flush_active_tlb_non_global(void)
{
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static bool x86_64_is_page_aligned(uint64_t value);
static bool x86_64_is_pa_encodable(phys_addr_t pa);

phys_addr_t arch_get_pt(void)
{
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uintptr_t) (cr3 & X86_64_PTE_ADDR_MASK);
}

void arch_set_pt(phys_addr_t pt)
{
    if (pt == 0)
    {
        kprintf("Arx kernel: arch_set_pt rejected null page table\n");
        return;
    }

    if (!x86_64_is_page_aligned((uint64_t) pt))
    {
        kprintf("Arx kernel: arch_set_pt rejected unaligned page table\n");
        return;
    }

    if (!x86_64_is_pa_encodable((uint64_t) pt))
    {
        kprintf("Arx kernel: arch_set_pt rejected non-encodable page table\n");
        return;
    }

    uint64_t old_cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));

    const uint64_t low_control_bits = old_cr3 & X86_64_PAGE_OFFSET_MASK;
    const uint64_t new_cr3          = (((uint64_t) pt) & X86_64_PTE_ADDR_MASK) | low_control_bits;
    if (new_cr3 == old_cr3)
    {
        return;
    }

    __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
}

static bool x86_64_is_page_aligned(uint64_t value)
{
    return (value & X86_64_PAGE_OFFSET_MASK) == 0;
}

static bool x86_64_is_pa_encodable(phys_addr_t pa)
{
    return (pa & ~X86_64_PTE_ADDR_MASK) == 0;
}

static bool x86_64_is_canonical_va(virt_addr_t va)
{
    const uint64_t high_bits = va & X86_64_CANONICAL_HIGH_MASK;
    return high_bits == 0 || high_bits == X86_64_CANONICAL_HIGH_MASK;
}

static bool x86_64_is_canonical_range(virt_addr_t va_start, uint64_t size)
{
    const virt_addr_t last_va = va_start + size - PAGE_SIZE;
    if (!x86_64_is_canonical_va(va_start) || !x86_64_is_canonical_va(last_va))
    {
        return false;
    }

    return (va_start & X86_64_CANONICAL_HIGH_MASK) == (last_va & X86_64_CANONICAL_HIGH_MASK);
}

static uint64_t x86_64_chunk_size(virt_addr_t va, uint64_t remaining, uint64_t level_shift)
{
    const uint64_t span   = 1ULL << level_shift;
    const uint64_t offset = va & (span - 1ULL);
    const uint64_t chunk  = span - offset;
    return remaining < chunk ? remaining : chunk;
}

static uint64_t* x86_64_table_from_pa(phys_addr_t pa)
{
    virt_addr_t table_va = pa_to_hhdm((uintptr_t) pa, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);
    return (uint64_t*) table_va;
}

static bool x86_64_decode_table_entry(uint64_t entry, phys_addr_t* table_pa)
{
    if ((entry & X86_64_PTE_PRESENT) == 0)
    {
        return false;
    }

    if ((entry & X86_64_PTE_PAGE_SIZE_OR_PAT) != 0)
    {
        return false;
    }

    const phys_addr_t pa = entry & X86_64_PTE_ADDR_MASK;
    if (!x86_64_is_page_aligned(pa))
    {
        return false;
    }

    *table_pa = pa;
    return true;
}

static bool x86_64_get_or_alloc_table(uint64_t* entry, uint64_t inherited_flags, phys_addr_t* table_pa)
{
    if ((*entry & X86_64_PTE_PRESENT) != 0)
    {
        return x86_64_decode_table_entry(*entry, table_pa);
    }

    void* new_table = pmm_alloc(PAGE_SIZE);
    if (new_table == NULL)
    {
        kprintf("Arx kernel: arch_map_page failed: unable to allocate page table\n");
        return false;
    }

    memset(new_table, 0, PAGE_SIZE);
    const phys_addr_t new_table_pa = hhdm_to_pa((uintptr_t) new_table, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);

    if (!x86_64_is_pa_encodable(new_table_pa))
    {
        kprintf("Arx kernel: arch_map_page failed: allocated table PA is not encodable\n");
        return false;
    }

    const uint64_t table_flags = X86_64_PTE_PRESENT | X86_64_PTE_WRITABLE | (inherited_flags & X86_64_PTE_USER);

    *entry    = (new_table_pa & X86_64_PTE_ADDR_MASK) | table_flags;
    *table_pa = new_table_pa;
    return true;
}

void __attribute__((weak)) arch_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, phys_addr_t page_table)
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

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_map_page rejected null page_table\n");
        return;
    }

    if (!x86_64_is_pa_encodable(pa) || !x86_64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_map_page rejected non-encodable PA/page_table\n");
        return;
    }

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;

    const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
    const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
    const uint64_t pd_index   = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
    const uint64_t pt_index   = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;

    uint64_t* pml4 = x86_64_table_from_pa((uint64_t) page_table);

    phys_addr_t pdpt_pa = 0;
    if (!x86_64_get_or_alloc_table(&pml4[pml4_index], sanitized_flags, &pdpt_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at PML4[%llu]\n", (unsigned long long) pml4_index);
        return;
    }
    uint64_t* pdpt = x86_64_table_from_pa(pdpt_pa);

    phys_addr_t pd_pa = 0;
    if (!x86_64_get_or_alloc_table(&pdpt[pdpt_index], sanitized_flags, &pd_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at PDPT[%llu]\n", (unsigned long long) pdpt_index);
        return;
    }
    uint64_t* pd = x86_64_table_from_pa(pd_pa);

    phys_addr_t pt_pa = 0;
    if (!x86_64_get_or_alloc_table(&pd[pd_index], sanitized_flags, &pt_pa))
    {
        kprintf("Arx kernel: arch_map_page failed at PD[%llu]\n", (unsigned long long) pd_index);
        return;
    }
    uint64_t* pt = x86_64_table_from_pa(pt_pa);

    const uint64_t new_pte = (pa & X86_64_PTE_ADDR_MASK) | sanitized_flags | X86_64_PTE_PRESENT;
    const uint64_t old_pte = pt[pt_index];
    if (old_pte == new_pte)
    {
        return;
    }

    pt[pt_index] = new_pte;
    if (page_table == arch_get_pt())
    {
        x86_64_invlpg(va);
    }
}

void __attribute__((weak)) arch_unmap_page(virt_addr_t va, phys_addr_t page_table)
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

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_unmap_page rejected null page_table\n");
        return;
    }

    if (!x86_64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_unmap_page rejected non-encodable page_table\n");
        return;
    }

    const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
    const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
    const uint64_t pd_index   = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
    const uint64_t pt_index   = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;

    uint64_t* pml4 = x86_64_table_from_pa((uint64_t) page_table);

    phys_addr_t pdpt_pa = 0;
    if (!x86_64_decode_table_entry(pml4[pml4_index], &pdpt_pa))
    {
        return;
    }

    uint64_t* pdpt = x86_64_table_from_pa(pdpt_pa);

    phys_addr_t pd_pa = 0;
    if (!x86_64_decode_table_entry(pdpt[pdpt_index], &pd_pa))
    {
        return;
    }

    uint64_t* pd = x86_64_table_from_pa(pd_pa);

    phys_addr_t pt_pa = 0;
    if (!x86_64_decode_table_entry(pd[pd_index], &pt_pa))
    {
        return;
    }

    uint64_t* pt      = x86_64_table_from_pa(pt_pa);
    uint64_t  old_pte = pt[pt_index];
    if ((old_pte & X86_64_PTE_PRESENT) == 0)
    {
        return;
    }

    pt[pt_index] = 0;
    if (page_table == arch_get_pt())
    {
        x86_64_invlpg(va);
    }
}

void __attribute__((weak)) arch_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, phys_addr_t page_table)
{
    if (size == 0)
    {
        return;
    }

    if (!x86_64_is_page_aligned(va_start) || !x86_64_is_page_aligned(pa_start) || !x86_64_is_page_aligned(size) || !x86_64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_map_range rejected unaligned VA/PA/size/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_map_range rejected null page_table\n");
        return;
    }

    if (va_start > UINT64_MAX - size || pa_start > UINT64_MAX - size)
    {
        kprintf("Arx kernel: arch_map_range rejected overflowing VA/PA range\n");
        return;
    }

    if (!x86_64_is_canonical_range(va_start, size))
    {
        kprintf("Arx kernel: arch_map_range rejected non-canonical VA range starting at 0x%llx\n", (unsigned long long) va_start);
        return;
    }

    const uint64_t    range_end = va_start + size;
    const phys_addr_t last_pa   = range_end == va_start ? pa_start : pa_start + size - PAGE_SIZE;
    if (!x86_64_is_pa_encodable(pa_start) || !x86_64_is_pa_encodable(last_pa) || !x86_64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_map_range rejected non-encodable PA/page_table\n");
        return;
    }

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;
    const bool     active_pt       = page_table == arch_get_pt();

    bool        any_changed         = false;
    bool        requires_page_flush = false;
    uint64_t*   pml4                = x86_64_table_from_pa((uint64_t) page_table);
    virt_addr_t va                  = va_start;
    phys_addr_t pa                  = pa_start;

    while (va < range_end)
    {
        const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
        const uint64_t pml4_end   = va + x86_64_chunk_size(va, range_end - va, X86_64_PT_SHIFT_PML4);

        phys_addr_t pdpt_pa = 0;
        if (!x86_64_get_or_alloc_table(&pml4[pml4_index], sanitized_flags, &pdpt_pa))
        {
            kprintf("Arx kernel: arch_map_range failed at PML4[%llu]\n", (unsigned long long) pml4_index);
            goto out;
        }
        uint64_t* pdpt = x86_64_table_from_pa(pdpt_pa);

        while (va < pml4_end)
        {
            const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
            const uint64_t pdpt_end   = va + x86_64_chunk_size(va, pml4_end - va, X86_64_PT_SHIFT_PDPT);

            phys_addr_t pd_pa = 0;
            if (!x86_64_get_or_alloc_table(&pdpt[pdpt_index], sanitized_flags, &pd_pa))
            {
                kprintf("Arx kernel: arch_map_range failed at PDPT[%llu]\n", (unsigned long long) pdpt_index);
                goto out;
            }
            uint64_t* pd = x86_64_table_from_pa(pd_pa);

            while (va < pdpt_end)
            {
                const uint64_t pd_index = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
                const uint64_t pt_end   = va + x86_64_chunk_size(va, pdpt_end - va, X86_64_PT_SHIFT_PD);

                phys_addr_t pt_pa = 0;
                if (!x86_64_get_or_alloc_table(&pd[pd_index], sanitized_flags, &pt_pa))
                {
                    kprintf("Arx kernel: arch_map_range failed at PD[%llu]\n", (unsigned long long) pd_index);
                    goto out;
                }
                uint64_t*      pt          = x86_64_table_from_pa(pt_pa);
                const uint64_t pt_index    = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;
                const uint64_t entry_count = (pt_end - va) >> PAGE_SHIFT;

                for (uint64_t entry = 0; entry < entry_count; ++entry)
                {
                    const phys_addr_t entry_pa = pa + (entry << PAGE_SHIFT);
                    const uint64_t    new_pte  = (entry_pa & X86_64_PTE_ADDR_MASK) | sanitized_flags | X86_64_PTE_PRESENT;
                    const uint64_t    old_pte  = pt[pt_index + entry];

                    if (old_pte == new_pte)
                    {
                        continue;
                    }

                    pt[pt_index + entry] = new_pte;
                    any_changed          = true;
                    if (((old_pte | new_pte) & X86_64_PTE_GLOBAL) != 0)
                    {
                        requires_page_flush = true;
                    }
                }

                pa += pt_end - va;
                va = pt_end;
            }
        }
    }

out:
    if (active_pt && any_changed)
    {
        if (requires_page_flush)
        {
            for (virt_addr_t flush_va = va_start; flush_va < range_end; flush_va += PAGE_SIZE)
            {
                x86_64_invlpg(flush_va);
            }
        }
        else
        {
            x86_64_flush_active_tlb_non_global();
        }
    }
}

void __attribute__((weak)) arch_unmap_range(virt_addr_t va_start, uint64_t size, phys_addr_t page_table)
{
    if (size == 0)
    {
        return;
    }

    if (!x86_64_is_page_aligned(va_start) || !x86_64_is_page_aligned(size) || !x86_64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_unmap_range rejected unaligned VA/size/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_unmap_range rejected null page_table\n");
        return;
    }

    if (va_start > UINT64_MAX - size)
    {
        kprintf("Arx kernel: arch_unmap_range rejected overflowing VA range\n");
        return;
    }

    if (!x86_64_is_canonical_range(va_start, size))
    {
        kprintf("Arx kernel: arch_unmap_range rejected non-canonical VA range starting at 0x%llx\n", (unsigned long long) va_start);
        return;
    }

    if (!x86_64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_unmap_range rejected non-encodable page_table\n");
        return;
    }

    const bool     active_pt           = page_table == arch_get_pt();
    const uint64_t range_end           = va_start + size;
    bool           any_changed         = false;
    bool           requires_page_flush = false;
    uint64_t*      pml4                = x86_64_table_from_pa((uint64_t) page_table);
    virt_addr_t    va                  = va_start;

    while (va < range_end)
    {
        const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
        const uint64_t pml4_end   = va + x86_64_chunk_size(va, range_end - va, X86_64_PT_SHIFT_PML4);

        phys_addr_t pdpt_pa = 0;
        if (!x86_64_decode_table_entry(pml4[pml4_index], &pdpt_pa))
        {
            va = pml4_end;
            continue;
        }
        uint64_t* pdpt = x86_64_table_from_pa(pdpt_pa);

        while (va < pml4_end)
        {
            const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
            const uint64_t pdpt_end   = va + x86_64_chunk_size(va, pml4_end - va, X86_64_PT_SHIFT_PDPT);

            phys_addr_t pd_pa = 0;
            if (!x86_64_decode_table_entry(pdpt[pdpt_index], &pd_pa))
            {
                va = pdpt_end;
                continue;
            }
            uint64_t* pd = x86_64_table_from_pa(pd_pa);

            while (va < pdpt_end)
            {
                const uint64_t pd_index = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
                const uint64_t pt_end   = va + x86_64_chunk_size(va, pdpt_end - va, X86_64_PT_SHIFT_PD);

                phys_addr_t pt_pa = 0;
                if (!x86_64_decode_table_entry(pd[pd_index], &pt_pa))
                {
                    va = pt_end;
                    continue;
                }
                uint64_t*      pt          = x86_64_table_from_pa(pt_pa);
                const uint64_t pt_index    = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;
                const uint64_t entry_count = (pt_end - va) >> PAGE_SHIFT;

                for (uint64_t entry = 0; entry < entry_count; ++entry)
                {
                    const uint64_t old_pte = pt[pt_index + entry];
                    if ((old_pte & X86_64_PTE_PRESENT) == 0)
                    {
                        continue;
                    }

                    pt[pt_index + entry] = 0;
                    any_changed          = true;
                    if ((old_pte & X86_64_PTE_GLOBAL) != 0)
                    {
                        requires_page_flush = true;
                    }
                }

                va = pt_end;
            }
        }
    }

    if (active_pt && any_changed)
    {
        if (requires_page_flush)
        {
            for (virt_addr_t flush_va = va_start; flush_va < range_end; flush_va += PAGE_SIZE)
            {
                x86_64_invlpg(flush_va);
            }
        }
        else
        {
            x86_64_flush_active_tlb_non_global();
        }
    }
}

void __attribute__((weak)) arch_protect(virt_addr_t va, uint64_t flags, phys_addr_t page_table)
{
    if (!x86_64_is_canonical_va(va))
    {
        kprintf("Arx kernel: arch_protect rejected non-canonical VA: 0x%llx\n", (unsigned long long) va);
        return;
    }

    if (!x86_64_is_page_aligned(va) || !x86_64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_protect rejected unaligned VA/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_protect rejected null page_table\n");
        return;
    }

    if (!x86_64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_protect rejected non-encodable page_table\n");
        return;
    }

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;

    const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
    const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
    const uint64_t pd_index   = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
    const uint64_t pt_index   = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;

    uint64_t* pml4 = x86_64_table_from_pa((uint64_t) page_table);

    phys_addr_t pdpt_pa = 0;
    if (!x86_64_decode_table_entry(pml4[pml4_index], &pdpt_pa))
    {
        return;
    }
    uint64_t* pdpt = x86_64_table_from_pa(pdpt_pa);

    phys_addr_t pd_pa = 0;
    if (!x86_64_decode_table_entry(pdpt[pdpt_index], &pd_pa))
    {
        return;
    }
    uint64_t* pd = x86_64_table_from_pa(pd_pa);

    phys_addr_t pt_pa = 0;
    if (!x86_64_decode_table_entry(pd[pd_index], &pt_pa))
    {
        return;
    }
    uint64_t* pt = x86_64_table_from_pa(pt_pa);

    const uint64_t old_pte = pt[pt_index];
    if ((old_pte & X86_64_PTE_PRESENT) == 0)
    {
        return;
    }

    const uint64_t new_pte = (old_pte & X86_64_PTE_ADDR_MASK) | sanitized_flags | X86_64_PTE_PRESENT;
    if (old_pte == new_pte)
    {
        return;
    }

    pt[pt_index] = new_pte;
    if (page_table == arch_get_pt())
    {
        x86_64_invlpg(va);
    }
}

void __attribute__((weak)) arch_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, phys_addr_t page_table)
{
    if (size == 0)
    {
        return;
    }

    if (!x86_64_is_page_aligned(va_start) || !x86_64_is_page_aligned(size) || !x86_64_is_page_aligned((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_protect_range rejected unaligned VA/size/page_table\n");
        return;
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: arch_protect_range rejected null page_table\n");
        return;
    }

    if (va_start > UINT64_MAX - size)
    {
        kprintf("Arx kernel: arch_protect_range rejected overflowing VA range\n");
        return;
    }

    if (!x86_64_is_canonical_range(va_start, size))
    {
        kprintf("Arx kernel: arch_protect_range rejected non-canonical VA range starting at 0x%llx\n", (unsigned long long) va_start);
        return;
    }

    if (!x86_64_is_pa_encodable((uint64_t) page_table))
    {
        kprintf("Arx kernel: arch_protect_range rejected non-encodable page_table\n");
        return;
    }

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;
    const bool     active_pt       = page_table == arch_get_pt();
    const uint64_t range_end       = va_start + size;

    bool        any_changed         = false;
    bool        requires_page_flush = false;
    uint64_t*   pml4                = x86_64_table_from_pa((uint64_t) page_table);
    virt_addr_t va                  = va_start;

    while (va < range_end)
    {
        const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
        const uint64_t pml4_end   = va + x86_64_chunk_size(va, range_end - va, X86_64_PT_SHIFT_PML4);

        phys_addr_t pdpt_pa = 0;
        if (!x86_64_decode_table_entry(pml4[pml4_index], &pdpt_pa))
        {
            va = pml4_end;
            continue;
        }
        uint64_t* pdpt = x86_64_table_from_pa(pdpt_pa);

        while (va < pml4_end)
        {
            const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
            const uint64_t pdpt_end   = va + x86_64_chunk_size(va, pml4_end - va, X86_64_PT_SHIFT_PDPT);

            phys_addr_t pd_pa = 0;
            if (!x86_64_decode_table_entry(pdpt[pdpt_index], &pd_pa))
            {
                va = pdpt_end;
                continue;
            }
            uint64_t* pd = x86_64_table_from_pa(pd_pa);

            while (va < pdpt_end)
            {
                const uint64_t pd_index = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
                const uint64_t pt_end   = va + x86_64_chunk_size(va, pdpt_end - va, X86_64_PT_SHIFT_PD);

                phys_addr_t pt_pa = 0;
                if (!x86_64_decode_table_entry(pd[pd_index], &pt_pa))
                {
                    va = pt_end;
                    continue;
                }
                uint64_t*      pt          = x86_64_table_from_pa(pt_pa);
                const uint64_t pt_index    = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;
                const uint64_t entry_count = (pt_end - va) >> PAGE_SHIFT;

                for (uint64_t entry = 0; entry < entry_count; ++entry)
                {
                    const uint64_t old_pte = pt[pt_index + entry];
                    if ((old_pte & X86_64_PTE_PRESENT) == 0)
                    {
                        continue;
                    }

                    const uint64_t new_pte = (old_pte & X86_64_PTE_ADDR_MASK) | sanitized_flags | X86_64_PTE_PRESENT;
                    if (old_pte == new_pte)
                    {
                        continue;
                    }

                    pt[pt_index + entry] = new_pte;
                    any_changed          = true;
                    if (((old_pte | new_pte) & X86_64_PTE_GLOBAL) != 0)
                    {
                        requires_page_flush = true;
                    }
                }

                va = pt_end;
            }
        }
    }

    if (active_pt && any_changed)
    {
        if (requires_page_flush)
        {
            for (virt_addr_t flush_va = va_start; flush_va < range_end; flush_va += PAGE_SIZE)
            {
                x86_64_invlpg(flush_va);
            }
        }
        else
        {
            x86_64_flush_active_tlb_non_global();
        }
    }
}

phys_addr_t __attribute__((weak)) arch_virt_to_phys(virt_addr_t va, phys_addr_t page_table)
{
    if (!x86_64_is_canonical_va(va))
    {
        return 0;
    }

    if (!x86_64_is_page_aligned((uint64_t) page_table) || page_table == 0 || !x86_64_is_pa_encodable((uint64_t) page_table))
    {
        return 0;
    }

    const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
    const uint64_t pdpt_index = (va >> X86_64_PT_SHIFT_PDPT) & X86_64_PT_INDEX_MASK;
    const uint64_t pd_index   = (va >> X86_64_PT_SHIFT_PD) & X86_64_PT_INDEX_MASK;
    const uint64_t pt_index   = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;

    uint64_t* pml4 = x86_64_table_from_pa((uint64_t) page_table);

    phys_addr_t pdpt_pa = 0;
    if (!x86_64_decode_table_entry(pml4[pml4_index], &pdpt_pa))
    {
        return 0;
    }
    uint64_t* pdpt = x86_64_table_from_pa(pdpt_pa);

    phys_addr_t pd_pa = 0;
    if (!x86_64_decode_table_entry(pdpt[pdpt_index], &pd_pa))
    {
        return 0;
    }
    uint64_t* pd = x86_64_table_from_pa(pd_pa);

    phys_addr_t pt_pa = 0;
    if (!x86_64_decode_table_entry(pd[pd_index], &pt_pa))
    {
        return 0;
    }
    uint64_t* pt = x86_64_table_from_pa(pt_pa);

    const uint64_t pte = pt[pt_index];
    if ((pte & X86_64_PTE_PRESENT) == 0)
    {
        return 0;
    }

    return (pte & X86_64_PTE_ADDR_MASK) | (va & X86_64_PAGE_OFFSET_MASK);
}
