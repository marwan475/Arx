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

void x86_64_invlpg(virt_addr_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

void x86_64_flush_active_tlb_non_global(void)
{
    uint64_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
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

static void tlb_shootdown(phys_addr_t page_table, virt_addr_t va_start, uint64_t size, bool requires_page_flush)
{
    const uint8_t self_cpu_id = arch_cpu_id();
    ipi_request_data_t request_data;

    request_data.type                        = IPI_REQUEST_INVALIDATE_TLB;
    request_data.tlb_invalidation.va_start   = va_start;
    request_data.tlb_invalidation.size       = size;
    request_data.tlb_invalidation.requires_page_flush = requires_page_flush;
    request_data.tlb_invalidation.tlb_invalidation_type = size == PAGE_SIZE ? IPI_TLB_INVALIDATE_SINGLE_PAGE : IPI_TLB_INVALIDATE_RANGE;

    for (uint8_t cpu_id = 0; cpu_id < dispatcher.cpu_count; ++cpu_id)
    {
        cpu_info_t* cpu_info = &dispatcher.cpus[cpu_id];

        if (cpu_id == self_cpu_id || !cpu_info->initialized)
        {
            continue;
        }

        if (cpu_info->address_space == NULL || cpu_info->address_space->pt != page_table)
        {
            continue;
        }

        // send ipi locks the targets ipi lock. its handler will release the lock
        send_ipi(cpu_id, (uint8_t) IPI_REQUEST_INVALIDATE_TLB, &request_data);

        // wait on targets ipi lock. the lock will be released after the ipi is handled
        spinlock_acquire(&cpu_info->ipi_lock);
        spinlock_release(&cpu_info->ipi_lock);
    }
}

static void x86_64_sync_tlb_single_page(virt_addr_t va, phys_addr_t page_table)
{
    if (page_table == arch_get_pt())
    {
        x86_64_invlpg(va);
    }

    tlb_shootdown(page_table, va, PAGE_SIZE, true);
}

static void x86_64_sync_tlb_range(virt_addr_t va_start, virt_addr_t range_end, bool any_changed, bool active_pt, bool requires_page_flush, phys_addr_t page_table)
{
    if (!any_changed)
    {
        return;
    }

    if (active_pt)
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

    tlb_shootdown(page_table, va_start, range_end - va_start, requires_page_flush);
}

static bool x86_64_validate_single_page_op(const char* op, virt_addr_t va, phys_addr_t page_table, bool requires_pa, phys_addr_t pa)
{
    if (!x86_64_is_canonical_va(va))
    {
        kprintf("Arx kernel: %s rejected non-canonical VA: 0x%llx\n", op, (unsigned long long) va);
        return false;
    }

    if (requires_pa)
    {
        if (!x86_64_is_page_aligned(va) || !x86_64_is_page_aligned(pa) || !x86_64_is_page_aligned((uint64_t) page_table))
        {
            kprintf("Arx kernel: %s rejected unaligned VA/PA/page_table\n", op);
            return false;
        }
    }
    else
    {
        if (!x86_64_is_page_aligned(va) || !x86_64_is_page_aligned((uint64_t) page_table))
        {
            kprintf("Arx kernel: %s rejected unaligned VA/page_table\n", op);
            return false;
        }
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: %s rejected null page_table\n", op);
        return false;
    }

    if (requires_pa)
    {
        if (!x86_64_is_pa_encodable(pa) || !x86_64_is_pa_encodable((uint64_t) page_table))
        {
            kprintf("Arx kernel: %s rejected non-encodable PA/page_table\n", op);
            return false;
        }
    }
    else
    {
        if (!x86_64_is_pa_encodable((uint64_t) page_table))
        {
            kprintf("Arx kernel: %s rejected non-encodable page_table\n", op);
            return false;
        }
    }

    return true;
}

static bool x86_64_validate_range_op(const char* op, virt_addr_t va_start, uint64_t size, phys_addr_t page_table, bool requires_pa, phys_addr_t pa_start)
{
    if (size == 0)
    {
        return false;
    }

    if (requires_pa)
    {
        if (!x86_64_is_page_aligned(va_start) || !x86_64_is_page_aligned(pa_start) || !x86_64_is_page_aligned(size) || !x86_64_is_page_aligned((uint64_t) page_table))
        {
            kprintf("Arx kernel: %s rejected unaligned VA/PA/size/page_table\n", op);
            return false;
        }
    }
    else
    {
        if (!x86_64_is_page_aligned(va_start) || !x86_64_is_page_aligned(size) || !x86_64_is_page_aligned((uint64_t) page_table))
        {
            kprintf("Arx kernel: %s rejected unaligned VA/size/page_table\n", op);
            return false;
        }
    }

    if (page_table == 0)
    {
        kprintf("Arx kernel: %s rejected null page_table\n", op);
        return false;
    }

    if (requires_pa)
    {
        if (va_start > UINT64_MAX - size || pa_start > UINT64_MAX - size)
        {
            kprintf("Arx kernel: %s rejected overflowing VA/PA range\n", op);
            return false;
        }
    }
    else if (va_start > UINT64_MAX - size)
    {
        kprintf("Arx kernel: %s rejected overflowing VA range\n", op);
        return false;
    }

    if (!x86_64_is_canonical_range(va_start, size))
    {
        kprintf("Arx kernel: %s rejected non-canonical VA range starting at 0x%llx\n", op, (unsigned long long) va_start);
        return false;
    }

    if (requires_pa)
    {
        const uint64_t    range_end = va_start + size;
        const phys_addr_t last_pa   = range_end == va_start ? pa_start : pa_start + size - PAGE_SIZE;
        if (!x86_64_is_pa_encodable(pa_start) || !x86_64_is_pa_encodable(last_pa) || !x86_64_is_pa_encodable((uint64_t) page_table))
        {
            kprintf("Arx kernel: %s rejected non-encodable PA/page_table\n", op);
            return false;
        }
    }
    else
    {
        if (!x86_64_is_pa_encodable((uint64_t) page_table))
        {
            kprintf("Arx kernel: %s rejected non-encodable page_table\n", op);
            return false;
        }
    }

    return true;
}

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

typedef enum x86_64_walk_op
{
    X86_64_WALK_OP_MAP,
    X86_64_WALK_OP_UNMAP,
    X86_64_WALK_OP_PROTECT,
} x86_64_walk_op_t;

static bool x86_64_walk_page_table(virt_addr_t va_start, uint64_t size, phys_addr_t page_table, bool allocate_tables, const char* failure_context, x86_64_walk_op_t op, phys_addr_t pa_start, uint64_t sanitized_flags, bool* any_changed, bool* requires_page_flush)
{
    const uint64_t range_end = va_start + size;
    uint64_t*      pml4      = x86_64_table_from_pa((uint64_t) page_table);
    virt_addr_t    va        = va_start;
    phys_addr_t    pa        = pa_start;

    while (va < range_end)
    {
        const uint64_t pml4_index = (va >> X86_64_PT_SHIFT_PML4) & X86_64_PT_INDEX_MASK;
        const uint64_t pml4_end   = va + x86_64_chunk_size(va, range_end - va, X86_64_PT_SHIFT_PML4);

        phys_addr_t pdpt_pa = 0;
        if (allocate_tables)
        {
            if (!x86_64_get_or_alloc_table(&pml4[pml4_index], sanitized_flags, &pdpt_pa))
            {
                if (failure_context != NULL)
                {
                    kprintf("Arx kernel: %s failed at PML4[%llu]\n", failure_context, (unsigned long long) pml4_index);
                }
                return false;
            }
        }
        else if (!x86_64_decode_table_entry(pml4[pml4_index], &pdpt_pa))
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
            if (allocate_tables)
            {
                if (!x86_64_get_or_alloc_table(&pdpt[pdpt_index], sanitized_flags, &pd_pa))
                {
                    if (failure_context != NULL)
                    {
                        kprintf("Arx kernel: %s failed at PDPT[%llu]\n", failure_context, (unsigned long long) pdpt_index);
                    }
                    return false;
                }
            }
            else if (!x86_64_decode_table_entry(pdpt[pdpt_index], &pd_pa))
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
                if (allocate_tables)
                {
                    if (!x86_64_get_or_alloc_table(&pd[pd_index], sanitized_flags, &pt_pa))
                    {
                        if (failure_context != NULL)
                        {
                            kprintf("Arx kernel: %s failed at PD[%llu]\n", failure_context, (unsigned long long) pd_index);
                        }
                        return false;
                    }
                }
                else if (!x86_64_decode_table_entry(pd[pd_index], &pt_pa))
                {
                    va = pt_end;
                    continue;
                }
                uint64_t*      pt          = x86_64_table_from_pa(pt_pa);
                const uint64_t pt_index    = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;
                const uint64_t entry_count = (pt_end - va) >> PAGE_SHIFT;

                for (uint64_t entry = 0; entry < entry_count; ++entry)
                {
                    uint64_t* pte = &pt[pt_index + entry];
                    uint64_t  old = *pte;

                    if (op == X86_64_WALK_OP_MAP)
                    {
                        const phys_addr_t entry_pa = pa + (entry << PAGE_SHIFT);
                        const uint64_t    new_pte  = (entry_pa & X86_64_PTE_ADDR_MASK) | sanitized_flags | X86_64_PTE_PRESENT;
                        if (old == new_pte)
                        {
                            continue;
                        }

                        *pte         = new_pte;
                        *any_changed = true;
                        if (((old | new_pte) & X86_64_PTE_GLOBAL) != 0)
                        {
                            *requires_page_flush = true;
                        }
                        continue;
                    }

                    if (op == X86_64_WALK_OP_UNMAP)
                    {
                        if ((old & X86_64_PTE_PRESENT) == 0)
                        {
                            continue;
                        }

                        *pte         = 0;
                        *any_changed = true;
                        if ((old & X86_64_PTE_GLOBAL) != 0)
                        {
                            *requires_page_flush = true;
                        }
                        continue;
                    }

                    if ((old & X86_64_PTE_PRESENT) == 0)
                    {
                        continue;
                    }

                    const uint64_t new_pte = (old & X86_64_PTE_ADDR_MASK) | sanitized_flags | X86_64_PTE_PRESENT;
                    if (old == new_pte)
                    {
                        continue;
                    }

                    *pte         = new_pte;
                    *any_changed = true;
                    if (((old | new_pte) & X86_64_PTE_GLOBAL) != 0)
                    {
                        *requires_page_flush = true;
                    }
                }

                pa += pt_end - va;
                va = pt_end;
            }
        }
    }

    return true;
}

void __attribute__((weak)) arch_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, phys_addr_t page_table)
{
    if (!x86_64_validate_single_page_op("arch_map_page", va, page_table, true, pa))
    {
        return;
    }

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;
    bool           any_changed     = false;
    bool           requires_flush  = false;

    (void) x86_64_walk_page_table(va, PAGE_SIZE, page_table, true, "arch_map_page", X86_64_WALK_OP_MAP, pa, sanitized_flags, &any_changed, &requires_flush);
    if (any_changed)
    {
        x86_64_sync_tlb_single_page(va, page_table);
    }
}

void __attribute__((weak)) arch_unmap_page(virt_addr_t va, phys_addr_t page_table)
{
    if (!x86_64_validate_single_page_op("arch_unmap_page", va, page_table, false, 0))
    {
        return;
    }

    bool any_changed    = false;
    bool requires_flush = false;

    (void) x86_64_walk_page_table(va, PAGE_SIZE, page_table, false, NULL, X86_64_WALK_OP_UNMAP, 0, 0, &any_changed, &requires_flush);
    if (any_changed)
    {
        x86_64_sync_tlb_single_page(va, page_table);
    }
}

void __attribute__((weak)) arch_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, phys_addr_t page_table)
{
    if (!x86_64_validate_range_op("arch_map_range", va_start, size, page_table, true, pa_start))
    {
        return;
    }

    const uint64_t range_end = va_start + size;

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;
    const bool     active_pt       = page_table == arch_get_pt();

    bool any_changed         = false;
    bool requires_page_flush = false;

    (void) x86_64_walk_page_table(va_start, size, page_table, true, "arch_map_range", X86_64_WALK_OP_MAP, pa_start, sanitized_flags, &any_changed, &requires_page_flush);

    x86_64_sync_tlb_range(va_start, range_end, any_changed, active_pt, requires_page_flush, page_table);
}

void __attribute__((weak)) arch_unmap_range(virt_addr_t va_start, uint64_t size, phys_addr_t page_table)
{
    if (!x86_64_validate_range_op("arch_unmap_range", va_start, size, page_table, false, 0))
    {
        return;
    }

    const bool     active_pt           = page_table == arch_get_pt();
    const uint64_t range_end           = va_start + size;
    bool any_changed         = false;
    bool requires_page_flush = false;

    (void) x86_64_walk_page_table(va_start, size, page_table, false, NULL, X86_64_WALK_OP_UNMAP, 0, 0, &any_changed, &requires_page_flush);

    x86_64_sync_tlb_range(va_start, range_end, any_changed, active_pt, requires_page_flush, page_table);
}

void __attribute__((weak)) arch_protect(virt_addr_t va, uint64_t flags, phys_addr_t page_table)
{
    if (!x86_64_validate_single_page_op("arch_protect", va, page_table, false, 0))
    {
        return;
    }

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;
    bool           any_changed     = false;
    bool           requires_flush  = false;

    (void) x86_64_walk_page_table(va, PAGE_SIZE, page_table, false, NULL, X86_64_WALK_OP_PROTECT, 0, sanitized_flags, &any_changed, &requires_flush);
    if (any_changed)
    {
        x86_64_sync_tlb_single_page(va, page_table);
    }
}

void __attribute__((weak)) arch_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, phys_addr_t page_table)
{
    if (!x86_64_validate_range_op("arch_protect_range", va_start, size, page_table, false, 0))
    {
        return;
    }

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;
    const bool     active_pt       = page_table == arch_get_pt();
    const uint64_t range_end       = va_start + size;

    bool any_changed         = false;
    bool requires_page_flush = false;

    (void) x86_64_walk_page_table(va_start, size, page_table, false, NULL, X86_64_WALK_OP_PROTECT, 0, sanitized_flags, &any_changed, &requires_page_flush);

    x86_64_sync_tlb_range(va_start, range_end, any_changed, active_pt, requires_page_flush, page_table);
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
