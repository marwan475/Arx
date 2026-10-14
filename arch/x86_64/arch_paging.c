#include <arch/arch.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <platform.h>

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

typedef bool (*x86_64_leaf_callback_t)(uint64_t* pte, virt_addr_t va, phys_addr_t pa, void* context, bool* any_changed, bool* requires_page_flush);

typedef enum x86_64_walk_level_result
{
    X86_64_WALK_LEVEL_READY,
    X86_64_WALK_LEVEL_SKIP,
    X86_64_WALK_LEVEL_ERROR,
} x86_64_walk_level_result_t;

typedef struct x86_64_walk_request
{
    virt_addr_t            va_start;
    uint64_t               size;
    phys_addr_t            page_table;
    bool                   allocate_tables;
    uint64_t               inherited_table_flags;
    const char*            failure_context;
    x86_64_leaf_callback_t leaf_callback;
    void*                  leaf_context;
    phys_addr_t            pa_start;
    bool*                  any_changed;
    bool*                  requires_page_flush;
    virt_addr_t*           failed_va;
} x86_64_walk_request_t;

typedef struct x86_64_walk_next_level_request
{
    uint64_t*   current_table;
    virt_addr_t va;
    uint64_t    parent_end;
    uint64_t    level_shift;
    bool        allocate_tables;
    uint64_t    inherited_table_flags;
    const char* failure_context;
    const char* level_name;
    uint64_t    next_end;
    uint64_t*   next_table;
} x86_64_walk_next_level_request_t;

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
    return high_bits == ((va & (1ULL << 47)) ? X86_64_CANONICAL_HIGH_MASK : 0);
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
    const uint8_t      self_cpu_id = arch_cpu_id();
    ipi_request_data_t request_data;

    request_data.type                                   = IPI_REQUEST_INVALIDATE_TLB;
    request_data.tlb_invalidation.va_start              = va_start;
    request_data.tlb_invalidation.size                  = size;
    request_data.tlb_invalidation.requires_page_flush   = requires_page_flush;
    request_data.tlb_invalidation.tlb_invalidation_type = size == PAGE_SIZE ? IPI_TLB_INVALIDATE_SINGLE_PAGE : IPI_TLB_INVALIDATE_RANGE;

    for (uint8_t cpu_id = 0; cpu_id < platform.cpu_count; ++cpu_id)
    {
        cpu_info_t* cpu_info = &platform.cpus[cpu_id];

        if (cpu_id == self_cpu_id || !__atomic_load_n(&cpu_info->initialized, __ATOMIC_ACQUIRE))
        {
            continue;
        }

        if (va_start <= CANONICAL_USER_END && (cpu_info->address_space == NULL || cpu_info->address_space->pt != page_table))
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
    if (page_table == arch_get_pt() || va > CANONICAL_USER_END)
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

    if (active_pt || va_start > CANONICAL_USER_END)
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
    virt_addr_t table_va = pa_to_hhdm((uintptr_t) pa, platform.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, platform.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);
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
        error_logger("Arx kernel: arch_map_page failed: unable to allocate page table\n");
        return false;
    }

    memset(new_table, 0, PAGE_SIZE);
    const phys_addr_t new_table_pa = hhdm_to_pa((uintptr_t) new_table, platform.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, platform.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);

    if (!x86_64_is_pa_encodable(new_table_pa))
    {
        error_logger("Arx kernel: arch_map_page failed: allocated table PA is not encodable\n");
        pmm_free(new_table);
        return false;
    }

    const uint64_t table_flags = X86_64_PTE_PRESENT | X86_64_PTE_WRITABLE | (inherited_flags & X86_64_PTE_USER);

    *entry    = (new_table_pa & X86_64_PTE_ADDR_MASK) | table_flags;
    *table_pa = new_table_pa;
    return true;
}

static bool x86_64_map_leaf_callback(uint64_t* pte, virt_addr_t va, phys_addr_t pa, void* context, bool* any_changed, bool* requires_page_flush)
{
    (void) va;

    const uint64_t sanitized_flags = *(const uint64_t*) context;
    const uint64_t old             = *pte;
    const uint64_t new_pte         = (pa & X86_64_PTE_ADDR_MASK) | sanitized_flags | X86_64_PTE_PRESENT;
    if (old == new_pte)
    {
        return true;
    }

    *pte         = new_pte;
    *any_changed = true;
    if (((old | new_pte) & X86_64_PTE_GLOBAL) != 0)
    {
        *requires_page_flush = true;
    }

    return true;
}

static bool x86_64_unmap_leaf_callback(uint64_t* pte, virt_addr_t va, phys_addr_t pa, void* context, bool* any_changed, bool* requires_page_flush)
{
    (void) va;
    (void) pa;
    (void) context;

    const uint64_t old = *pte;
    if ((old & X86_64_PTE_PRESENT) == 0)
    {
        return true;
    }

    *pte         = 0;
    *any_changed = true;
    if ((old & X86_64_PTE_GLOBAL) != 0)
    {
        *requires_page_flush = true;
    }

    return true;
}

static bool x86_64_protect_leaf_callback(uint64_t* pte, virt_addr_t va, phys_addr_t pa, void* context, bool* any_changed, bool* requires_page_flush)
{
    (void) va;
    (void) pa;

    const uint64_t old = *pte;
    if ((old & X86_64_PTE_PRESENT) == 0)
    {
        return true;
    }

    const uint64_t sanitized_flags = *(const uint64_t*) context;
    const uint64_t new_pte         = (old & X86_64_PTE_ADDR_MASK) | sanitized_flags | X86_64_PTE_PRESENT;
    if (old == new_pte)
    {
        return true;
    }

    *pte         = new_pte;
    *any_changed = true;
    if (((old | new_pte) & X86_64_PTE_GLOBAL) != 0)
    {
        *requires_page_flush = true;
    }

    return true;
}

static x86_64_walk_level_result_t x86_64_walk_next_level(x86_64_walk_next_level_request_t* request)
{
    const uint64_t level_index = (request->va >> request->level_shift) & X86_64_PT_INDEX_MASK;
    request->next_end          = request->va + x86_64_chunk_size(request->va, request->parent_end - request->va, request->level_shift);

    uint64_t* parent = &request->current_table[level_index];
    if ((*parent & (X86_64_PTE_PRESENT | X86_64_PTE_PAGE_SIZE_OR_PAT)) == (X86_64_PTE_PRESENT | X86_64_PTE_PAGE_SIZE_OR_PAT))
    {
        if (request->level_shift != 30 && request->level_shift != 21)
            return X86_64_WALK_LEVEL_ERROR;
        uint64_t* children = pmm_alloc(PAGE_SIZE);
        if (children == NULL)
            return X86_64_WALK_LEVEL_ERROR;
        const uint64_t old         = *parent;
        const uint64_t child_shift = request->level_shift - 9;
        const uint64_t base        = old & X86_64_PTE_ADDR_MASK & ~((1ULL << request->level_shift) - 1);
        uint64_t       flags       = old & ~X86_64_PTE_ADDR_MASK;
        if (child_shift == 12)
        {
            flags &= ~X86_64_PTE_PAGE_SIZE_OR_PAT;
            if (old & (1ULL << 12))
                flags |= X86_64_PTE_PAGE_SIZE_OR_PAT;
        }
        else if (old & (1ULL << 12))
            flags |= (1ULL << 12);
        for (uint64_t i = 0; i < X86_64_PT_ENTRIES; ++i)
            children[i] = (base + (i << child_shift)) | flags;
        const zone_t* zone = &platform.cpus[arch_cpu_id()].numa_node->zone;
        *parent            = hhdm_to_pa((uintptr_t) children, zone->hhdm_present, zone->hhdm_offset) | X86_64_PTE_PRESENT | X86_64_PTE_WRITABLE | (old & X86_64_PTE_USER);
    }
    phys_addr_t next_table_pa = 0;
    if (request->allocate_tables)
    {
        if (!x86_64_get_or_alloc_table(&request->current_table[level_index], request->inherited_table_flags, &next_table_pa))
        {
            if (request->failure_context != NULL)
            {
                error_logger("Arx kernel: %s failed at %s[%llu]\n", request->failure_context, request->level_name, (unsigned long long) level_index);
            }
            return X86_64_WALK_LEVEL_ERROR;
        }
    }
    else if (!x86_64_decode_table_entry(request->current_table[level_index], &next_table_pa))
    {
        return X86_64_WALK_LEVEL_SKIP;
    }

    request->next_table = x86_64_table_from_pa(next_table_pa);
    return X86_64_WALK_LEVEL_READY;
}

static bool x86_64_walk_page_table(const x86_64_walk_request_t* request)
{
    const virt_addr_t            va_start              = request->va_start;
    const uint64_t               size                  = request->size;
    const phys_addr_t            page_table            = request->page_table;
    const bool                   allocate_tables       = request->allocate_tables;
    const uint64_t               inherited_table_flags = request->inherited_table_flags;
    const char*                  failure_context       = request->failure_context;
    const x86_64_leaf_callback_t leaf_callback         = request->leaf_callback;
    void*                        leaf_context          = request->leaf_context;
    bool*                        any_changed           = request->any_changed;
    bool*                        requires_page_flush   = request->requires_page_flush;

    const uint64_t range_end = va_start + size;
    uint64_t*      pml4      = x86_64_table_from_pa((uint64_t) page_table);
    virt_addr_t    va        = va_start;
    phys_addr_t    pa        = request->pa_start;

    while (va < range_end)
    {
        x86_64_walk_next_level_request_t pml4_request = {
                .current_table         = pml4,
                .va                    = va,
                .parent_end            = range_end,
                .level_shift           = X86_64_PT_SHIFT_PML4,
                .allocate_tables       = allocate_tables,
                .inherited_table_flags = inherited_table_flags,
                .failure_context       = failure_context,
                .level_name            = "PML4",
                .next_end              = 0,
                .next_table            = NULL,
        };
        const x86_64_walk_level_result_t pml4_result = x86_64_walk_next_level(&pml4_request);
        if (pml4_result == X86_64_WALK_LEVEL_ERROR)
        {
            if (request->failed_va != NULL)
                *request->failed_va = va;
            return false;
        }
        if (pml4_result == X86_64_WALK_LEVEL_SKIP)
        {
            va = pml4_request.next_end;
            continue;
        }
        const uint64_t pml4_end = pml4_request.next_end;
        uint64_t*      pdpt     = pml4_request.next_table;

        while (va < pml4_end)
        {
            x86_64_walk_next_level_request_t pdpt_request = {
                    .current_table         = pdpt,
                    .va                    = va,
                    .parent_end            = pml4_end,
                    .level_shift           = X86_64_PT_SHIFT_PDPT,
                    .allocate_tables       = allocate_tables,
                    .inherited_table_flags = inherited_table_flags,
                    .failure_context       = failure_context,
                    .level_name            = "PDPT",
                    .next_end              = 0,
                    .next_table            = NULL,
            };
            const x86_64_walk_level_result_t pdpt_result = x86_64_walk_next_level(&pdpt_request);
            if (pdpt_result == X86_64_WALK_LEVEL_ERROR)
            {
                if (request->failed_va != NULL)
                    *request->failed_va = va;
                return false;
            }
            if (pdpt_result == X86_64_WALK_LEVEL_SKIP)
            {
                va = pdpt_request.next_end;
                continue;
            }
            const uint64_t pdpt_end = pdpt_request.next_end;
            uint64_t*      pd       = pdpt_request.next_table;

            while (va < pdpt_end)
            {
                x86_64_walk_next_level_request_t pd_request = {
                        .current_table         = pd,
                        .va                    = va,
                        .parent_end            = pdpt_end,
                        .level_shift           = X86_64_PT_SHIFT_PD,
                        .allocate_tables       = allocate_tables,
                        .inherited_table_flags = inherited_table_flags,
                        .failure_context       = failure_context,
                        .level_name            = "PD",
                        .next_end              = 0,
                        .next_table            = NULL,
                };
                const x86_64_walk_level_result_t pd_result = x86_64_walk_next_level(&pd_request);
                if (pd_result == X86_64_WALK_LEVEL_ERROR)
                {
                    if (request->failed_va != NULL)
                        *request->failed_va = va;
                    return false;
                }
                if (pd_result == X86_64_WALK_LEVEL_SKIP)
                {
                    va = pd_request.next_end;
                    continue;
                }
                const uint64_t pt_end      = pd_request.next_end;
                uint64_t*      pt          = pd_request.next_table;
                const uint64_t pt_index    = (va >> X86_64_PT_SHIFT_PT) & X86_64_PT_INDEX_MASK;
                const uint64_t entry_count = (pt_end - va) >> PAGE_SHIFT;

                for (uint64_t entry = 0; entry < entry_count; ++entry)
                {
                    uint64_t*         pte     = &pt[pt_index + entry];
                    const virt_addr_t leaf_va = va + (entry << PAGE_SHIFT);
                    const phys_addr_t leaf_pa = pa + (entry << PAGE_SHIFT);

                    if (!leaf_callback(pte, leaf_va, leaf_pa, leaf_context, any_changed, requires_page_flush))
                    {
                        if (failure_context != NULL)
                        {
                            error_logger("Arx kernel: %s failed at PT[%llu] for VA 0x%llx\n", failure_context, (unsigned long long) (pt_index + entry), (unsigned long long) leaf_va);
                        }
                        return false;
                    }
                }

                pa += pt_end - va;
                va = pt_end;
            }
        }
    }

    return true;
}

static bool prune_empty_tables(phys_addr_t table, unsigned depth, virt_addr_t failed_va, phys_addr_t* retired, unsigned* count)
{
    uint64_t*      words = x86_64_table_from_pa(table);
    const unsigned index = (failed_va >> (12 + 9 * (depth - 1))) & X86_64_PT_INDEX_MASK;
    const uint64_t entry = words[index];
    if (depth > 1 && (entry & X86_64_PTE_PRESENT) && !(entry & X86_64_PTE_PAGE_SIZE_OR_PAT) && prune_empty_tables(entry & X86_64_PTE_ADDR_MASK, depth - 1, failed_va, retired, count) && !(depth == 4 && index >= 256))
    {
        words[index]        = 0;
        retired[(*count)++] = entry & X86_64_PTE_ADDR_MASK;
    }
    for (unsigned i = 0; i < 512; ++i)
        if (words[i] & X86_64_PTE_PRESENT)
            return false;
    return true;
}

static void rollback_empty_tables(phys_addr_t page_table, virt_addr_t failed_va)
{
    // One failed leaf descent can allocate at most three intermediate tables.
    // Keep detached tables intact until all paging-structure caches are flushed.
    phys_addr_t retired[3];
    unsigned    count = 0;
    prune_empty_tables(page_table, 4, failed_va, retired, &count);
    if (count != 0)
    {
        x86_64_flush_active_tlb_non_global();
        tlb_shootdown(page_table, CANONICAL_KERNEL_BASE, 2 * PAGE_SIZE, false);
        for (unsigned i = 0; i < count; ++i)
            pmm_free(x86_64_table_from_pa(retired[i]));
    }
}

bool __attribute__((weak)) arch_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, phys_addr_t page_table)
{
    if (!x86_64_validate_single_page_op("arch_map_page", va, page_table, true, pa))
    {
        return false;
    }

    const uint64_t        sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;
    bool                  any_changed     = false;
    bool                  requires_flush  = false;
    x86_64_walk_request_t walk_request    = {
            .va_start              = va,
            .size                  = PAGE_SIZE,
            .page_table            = page_table,
            .allocate_tables       = true,
            .inherited_table_flags = sanitized_flags,
            .failure_context       = "arch_map_page",
            .leaf_callback         = x86_64_map_leaf_callback,
            .leaf_context          = (void*) &sanitized_flags,
            .pa_start              = pa,
            .any_changed           = &any_changed,
            .requires_page_flush   = &requires_flush,
    };

    const bool success = x86_64_walk_page_table(&walk_request);
    if (any_changed)
    {
        x86_64_sync_tlb_single_page(va, page_table);
    }
    if (!success)
        rollback_empty_tables(page_table, va);
    return success;
}

void __attribute__((weak)) arch_unmap_page(virt_addr_t va, phys_addr_t page_table)
{
    if (!x86_64_validate_single_page_op("arch_unmap_page", va, page_table, false, 0))
    {
        return;
    }

    bool                  any_changed    = false;
    bool                  requires_flush = false;
    x86_64_walk_request_t walk_request   = {
            .va_start              = va,
            .size                  = PAGE_SIZE,
            .page_table            = page_table,
            .allocate_tables       = false,
            .inherited_table_flags = 0,
            .failure_context       = NULL,
            .leaf_callback         = x86_64_unmap_leaf_callback,
            .leaf_context          = NULL,
            .pa_start              = 0,
            .any_changed           = &any_changed,
            .requires_page_flush   = &requires_flush,
    };

    (void) x86_64_walk_page_table(&walk_request);
    if (any_changed)
    {
        x86_64_sync_tlb_single_page(va, page_table);
    }
}

bool __attribute__((weak)) arch_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, phys_addr_t page_table)
{
    if (!x86_64_validate_range_op("arch_map_range", va_start, size, page_table, true, pa_start))
    {
        return false;
    }

    const uint64_t range_end = va_start + size;
    virt_addr_t    failed_va = va_start;

    const uint64_t sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;
    const bool     active_pt       = page_table == arch_get_pt();

    bool                  any_changed         = false;
    bool                  requires_page_flush = false;
    x86_64_walk_request_t walk_request        = {
            .va_start              = va_start,
            .size                  = size,
            .page_table            = page_table,
            .allocate_tables       = true,
            .inherited_table_flags = sanitized_flags,
            .failure_context       = "arch_map_range",
            .failed_va             = &failed_va,
            .leaf_callback         = x86_64_map_leaf_callback,
            .leaf_context          = (void*) &sanitized_flags,
            .pa_start              = pa_start,
            .any_changed           = &any_changed,
            .requires_page_flush   = &requires_page_flush,
    };

    const bool success = x86_64_walk_page_table(&walk_request);

    x86_64_sync_tlb_range(va_start, range_end, any_changed, active_pt, requires_page_flush, page_table);
    if (!success)
        rollback_empty_tables(page_table, failed_va);
    return success;
}

void __attribute__((weak)) arch_unmap_range(virt_addr_t va_start, uint64_t size, phys_addr_t page_table)
{
    if (!x86_64_validate_range_op("arch_unmap_range", va_start, size, page_table, false, 0))
    {
        return;
    }

    const bool            active_pt           = page_table == arch_get_pt();
    const uint64_t        range_end           = va_start + size;
    bool                  any_changed         = false;
    bool                  requires_page_flush = false;
    x86_64_walk_request_t walk_request        = {
            .va_start              = va_start,
            .size                  = size,
            .page_table            = page_table,
            .allocate_tables       = false,
            .inherited_table_flags = 0,
            .failure_context       = NULL,
            .leaf_callback         = x86_64_unmap_leaf_callback,
            .leaf_context          = NULL,
            .pa_start              = 0,
            .any_changed           = &any_changed,
            .requires_page_flush   = &requires_page_flush,
    };

    (void) x86_64_walk_page_table(&walk_request);

    x86_64_sync_tlb_range(va_start, range_end, any_changed, active_pt, requires_page_flush, page_table);
}

void __attribute__((weak)) arch_protect(virt_addr_t va, uint64_t flags, phys_addr_t page_table)
{
    if (!x86_64_validate_single_page_op("arch_protect", va, page_table, false, 0))
    {
        return;
    }

    const uint64_t        sanitized_flags = flags & X86_64_PTE_ALLOWED_MAP_FLAGS;
    bool                  any_changed     = false;
    bool                  requires_flush  = false;
    x86_64_walk_request_t walk_request    = {
            .va_start              = va,
            .size                  = PAGE_SIZE,
            .page_table            = page_table,
            .allocate_tables       = false,
            .inherited_table_flags = 0,
            .failure_context       = NULL,
            .leaf_callback         = x86_64_protect_leaf_callback,
            .leaf_context          = (void*) &sanitized_flags,
            .pa_start              = 0,
            .any_changed           = &any_changed,
            .requires_page_flush   = &requires_flush,
    };

    (void) x86_64_walk_page_table(&walk_request);
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

    bool                  any_changed         = false;
    bool                  requires_page_flush = false;
    x86_64_walk_request_t walk_request        = {
            .va_start              = va_start,
            .size                  = size,
            .page_table            = page_table,
            .allocate_tables       = false,
            .inherited_table_flags = 0,
            .failure_context       = NULL,
            .leaf_callback         = x86_64_protect_leaf_callback,
            .leaf_context          = (void*) &sanitized_flags,
            .pa_start              = 0,
            .any_changed           = &any_changed,
            .requires_page_flush   = &requires_page_flush,
    };

    (void) x86_64_walk_page_table(&walk_request);

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

    const uint64_t pdpte = pdpt[pdpt_index];
    if ((pdpte & (X86_64_PTE_PRESENT | X86_64_PTE_PAGE_SIZE_OR_PAT)) == (X86_64_PTE_PRESENT | X86_64_PTE_PAGE_SIZE_OR_PAT))
        return (pdpte & X86_64_PTE_ADDR_MASK & ~((1ULL << 30) - 1)) | (va & ((1ULL << 30) - 1));
    phys_addr_t pd_pa = 0;
    if (!x86_64_decode_table_entry(pdpte, &pd_pa))
    {
        return 0;
    }
    uint64_t* pd = x86_64_table_from_pa(pd_pa);

    const uint64_t pde = pd[pd_index];
    if ((pde & (X86_64_PTE_PRESENT | X86_64_PTE_PAGE_SIZE_OR_PAT)) == (X86_64_PTE_PRESENT | X86_64_PTE_PAGE_SIZE_OR_PAT))
        return (pde & X86_64_PTE_ADDR_MASK & ~((1ULL << 21) - 1)) | (va & ((1ULL << 21) - 1));
    phys_addr_t pt_pa = 0;
    if (!x86_64_decode_table_entry(pde, &pt_pa))
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

phys_addr_t arch_create_user_page_table(void)
{
    void* root = pmm_alloc(PAGE_SIZE);
    if (!root)
        return 0;
    memset(root, 0, PAGE_SIZE);
    virt_addr_space_t* kernel = vmm_kernel_space();
    spinlock_acquire_preempt(&kernel->lock);
    uint64_t* source = x86_64_table_from_pa(kernel->pt);
    // User roots share stable upper-half parents, including currently empty
    // slots where a later kernel allocation may install a mapping.
    for (unsigned i = 256; i < 512; ++i)
    {
        phys_addr_t child;
        if (!x86_64_get_or_alloc_table(&source[i], 0, &child))
        {
            spinlock_release_preempt(&kernel->lock);
            pmm_free(root);
            return 0;
        }
    }
    memcpy((uint64_t*) root + 256, source + 256, PAGE_SIZE / 2);
    spinlock_release_preempt(&kernel->lock);
    return hhdm_to_pa((uintptr_t) root, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
}
static void destroy_empty_user_tables(phys_addr_t table, unsigned depth, unsigned entries)
{
    uint64_t* words = x86_64_table_from_pa(table);
    if (depth > 1)
        for (unsigned i = 0; i < entries; ++i)
            if ((words[i] & X86_64_PTE_PRESENT) && !(words[i] & (1ULL << 7)))
                destroy_empty_user_tables(words[i] & X86_64_PTE_ADDR_MASK, depth - 1, 512);
    pmm_free(words);
}
void arch_destroy_user_page_table(phys_addr_t page_table)
{
    if (page_table && page_table != arch_get_pt())
        destroy_empty_user_tables(page_table, 4, 256);
}

bool arch_user_page_accessible(virt_addr_t va, phys_addr_t page_table, bool write)
{
    if (va > CANONICAL_USER_END || !page_table || !x86_64_is_page_aligned(page_table) || !x86_64_is_pa_encodable(page_table))
    {
        return false;
    }
    phys_addr_t table = page_table;
    for (int shift = 39; shift >= 12; shift -= 9)
    {
        const uint64_t entry = x86_64_table_from_pa(table)[(va >> shift) & X86_64_PT_INDEX_MASK];
        if ((entry & (X86_64_PTE_PRESENT | X86_64_PTE_USER)) != (X86_64_PTE_PRESENT | X86_64_PTE_USER) || (write && (entry & X86_64_PTE_WRITABLE) == 0))
        {
            return false;
        }
        if (shift == 12 || ((shift == 30 || shift == 21) && (entry & X86_64_PTE_PAGE_SIZE_OR_PAT)))
        {
            return true;
        }
        if (!x86_64_decode_table_entry(entry, &table))
        {
            return false;
        }
    }
    return false;
}
