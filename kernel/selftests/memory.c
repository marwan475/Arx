// Ai generated testing
// Not thread safe

#include <memory/pmm.h>
#include <memory/vmm.h>
#include <memory/heap.h>
#include <selftests/selftests.h>

#define PMM_TEST_MAX_PTRS 2048
#define PMM_TEST_SCENARIOS 10

typedef struct pmm_test_alloc
{
    void*     ptr;
    uintptr_t pa_start;
    uintptr_t pa_end;
    size_t    requested;
    size_t    actual;
} pmm_test_alloc_t;

static size_t pmm_test_round_up_pow2_pages(size_t size)
{
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t pow2  = 1;

    while (pow2 < pages)
    {
        pow2 <<= 1;
    }

    return pow2;
}

static void pmm_test_log_fail(const char* message, size_t* failures)
{
    (*failures)++;
    kprintf("Arx kernel: pmm_test FAIL: %s\n", message);
}

static bool pmm_test_is_page_aligned(const void* ptr)
{
    return (((uintptr_t) ptr) & (PAGE_SIZE - 1)) == 0;
}

static bool pmm_test_check_zone_accounting(const zone_t* zone)
{
    return (zone->free_pages + zone->used_pages) == zone->total_pages;
}

static void vmm_test_log_fail(const char* message, size_t* failures)
{
    (*failures)++;
    kprintf("Arx kernel: vmm_test FAIL: %s\n", message);
}

static bool vmm_test_find_unmapped_window(virt_addr_t* va_out)
{
    const virt_addr_t candidates[] = {
            0xFFFFF00000000000ULL,
            0xFFFFE00000000000ULL,
            0xFFFFC00000000000ULL,
            0x0000004000000000ULL,
    };

    for (size_t i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); i++)
    {
        virt_addr_t va = align_down(candidates[i], PAGE_SIZE);
        if (vmm_virt_to_phys(va, dispatcher.cpus[arch_cpu_id()].address_space) == 0 && vmm_virt_to_phys(va + PAGE_SIZE, dispatcher.cpus[arch_cpu_id()].address_space) == 0)
        {
            *va_out = va;
            return true;
        }
    }

    return false;
}

static size_t vmm_test_count_regions(virt_region_t* head)
{
    size_t count = 0;
    for (virt_region_t* it = head; it != NULL; it = it->next)
    {
        count++;
    }
    return count;
}

static virt_region_t* vmm_test_find_region_by_start(virt_region_t* head, virt_addr_t start)
{
    for (virt_region_t* it = head; it != NULL; it = it->next)
    {
        if (it->start == start)
        {
            return it;
        }
    }
    return NULL;
}

static void vmm_test(void)
{
    size_t failures = 0;
    size_t passes   = 0;

    const size_t kernel_used_before = vmm_test_count_regions(dispatcher.cpus[arch_cpu_id()].address_space->kernel_used_regions);
    const size_t kernel_free_before = vmm_test_count_regions(dispatcher.cpus[arch_cpu_id()].address_space->kernel_free_regions);

    kprintf("Arx kernel: vmm_test start\n");

    void* range_block_va = pmm_alloc(PAGE_SIZE * 2ULL);
    if (range_block_va == NULL)
    {
        vmm_test_log_fail("failed to allocate PMM pages for VMM test", &failures);
        kprintf("Arx kernel: vmm_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
        kprintf("Arx kernel: vmm_test RESULT=FAIL\n");
        return;
    }

    void* page_a_va = range_block_va;
    void* page_b_va = (void*) ((uintptr_t) range_block_va + PAGE_SIZE);

    phys_addr_t page_a_pa = hhdm_to_pa((uintptr_t) page_a_va, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);
    phys_addr_t page_b_pa = page_a_pa + PAGE_SIZE;

    virt_addr_t test_va = 0;
    if (!vmm_test_find_unmapped_window(&test_va))
    {
        vmm_test_log_fail("could not find an unmapped virtual window for test", &failures);
        pmm_free(range_block_va);
        kprintf("Arx kernel: vmm_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
        kprintf("Arx kernel: vmm_test RESULT=FAIL\n");
        return;
    }
    passes++;

    uint64_t map_flags = 0;
    ARCH_PAGE_FLAGS_INIT(map_flags);
    ARCH_PAGE_FLAG_SET_READ(map_flags);
    ARCH_PAGE_FLAG_SET_WRITE(map_flags);

    vmm_map_page(test_va, page_a_pa, map_flags, dispatcher.cpus[arch_cpu_id()].address_space);
    if (vmm_virt_to_phys(test_va, dispatcher.cpus[arch_cpu_id()].address_space) != page_a_pa)
    {
        vmm_test_log_fail("map_page translation mismatch", &failures);
    }
    else
    {
        passes++;
    }

    vmm_protect_page(test_va, map_flags, dispatcher.cpus[arch_cpu_id()].address_space);
    if (vmm_virt_to_phys(test_va, dispatcher.cpus[arch_cpu_id()].address_space) != page_a_pa)
    {
        vmm_test_log_fail("protect_page changed physical mapping", &failures);
    }
    else
    {
        passes++;
    }

    vmm_map_range(test_va, page_a_pa, PAGE_SIZE * 2ULL, map_flags, dispatcher.cpus[arch_cpu_id()].address_space);
    if (vmm_virt_to_phys(test_va, dispatcher.cpus[arch_cpu_id()].address_space) != page_a_pa || vmm_virt_to_phys(test_va + PAGE_SIZE, dispatcher.cpus[arch_cpu_id()].address_space) != page_b_pa)
    {
        vmm_test_log_fail("map_range translation mismatch", &failures);
    }
    else
    {
        passes++;
    }

    vmm_protect_range(test_va, PAGE_SIZE * 2ULL, map_flags, dispatcher.cpus[arch_cpu_id()].address_space);
    if (vmm_virt_to_phys(test_va, dispatcher.cpus[arch_cpu_id()].address_space) != page_a_pa || vmm_virt_to_phys(test_va + PAGE_SIZE, dispatcher.cpus[arch_cpu_id()].address_space) != page_b_pa)
    {
        vmm_test_log_fail("protect_range changed physical mapping", &failures);
    }
    else
    {
        passes++;
    }

    vmm_unmap_page(test_va, dispatcher.cpus[arch_cpu_id()].address_space);
    if (vmm_virt_to_phys(test_va, dispatcher.cpus[arch_cpu_id()].address_space) != 0)
    {
        vmm_test_log_fail("unmap_page did not clear mapping", &failures);
    }
    else
    {
        passes++;
    }

    vmm_map_page(test_va, page_a_pa, map_flags, dispatcher.cpus[arch_cpu_id()].address_space);

    vmm_unmap_range(test_va, PAGE_SIZE * 2ULL, dispatcher.cpus[arch_cpu_id()].address_space);
    if (vmm_virt_to_phys(test_va, dispatcher.cpus[arch_cpu_id()].address_space) != 0 || vmm_virt_to_phys(test_va + PAGE_SIZE, dispatcher.cpus[arch_cpu_id()].address_space) != 0)
    {
        vmm_test_log_fail("unmap_range did not clear mappings", &failures);
    }
    else
    {
        passes++;
    }

    vmm_switch_addr_space(dispatcher.cpus[arch_cpu_id()].address_space);
    passes++;

    const size_t reserve_size         = PAGE_SIZE + 123;
    const size_t reserve_size_aligned = align_up(reserve_size, PAGE_SIZE);

    virt_addr_t reserved = vmm_reserve_region(dispatcher.cpus[arch_cpu_id()].address_space, reserve_size, VIRT_ADDR_KERNEL);
    if (reserved == 0)
    {
        vmm_test_log_fail("reserve_region returned 0 for kernel allocation", &failures);
    }
    else
    {
        passes++;
    }

    if ((reserved & (PAGE_SIZE - 1)) != 0)
    {
        vmm_test_log_fail("reserve_region returned non-page-aligned address", &failures);
    }
    else
    {
        passes++;
    }

    virt_region_t* used_reserved = vmm_test_find_region_by_start(dispatcher.cpus[arch_cpu_id()].address_space->kernel_used_regions, reserved);
    if (used_reserved == NULL)
    {
        vmm_test_log_fail("reserved region not found in kernel used list", &failures);
    }
    else if (used_reserved->size != reserve_size_aligned || used_reserved->type != VIRT_ADDR_KERNEL)
    {
        vmm_test_log_fail("reserved kernel used region metadata mismatch", &failures);
    }
    else
    {
        passes++;
    }

    if (vmm_test_find_region_by_start(dispatcher.cpus[arch_cpu_id()].address_space->kernel_free_regions, reserved) != NULL)
    {
        vmm_test_log_fail("reserved region incorrectly present in kernel free list", &failures);
    }
    else
    {
        passes++;
    }

    const size_t kernel_used_after_reserve = vmm_test_count_regions(dispatcher.cpus[arch_cpu_id()].address_space->kernel_used_regions);
    if (kernel_used_after_reserve != (kernel_used_before + 1))
    {
        vmm_test_log_fail("kernel used list count did not increase after reserve", &failures);
    }
    else
    {
        passes++;
    }

    vmm_free_region(dispatcher.cpus[arch_cpu_id()].address_space, reserved);

    if (vmm_test_find_region_by_start(dispatcher.cpus[arch_cpu_id()].address_space->kernel_used_regions, reserved) != NULL)
    {
        vmm_test_log_fail("freed region still present in kernel used list", &failures);
    }
    else
    {
        passes++;
    }

    if (vmm_test_find_region_by_start(dispatcher.cpus[arch_cpu_id()].address_space->kernel_free_regions, reserved) == NULL)
    {
        vmm_test_log_fail("freed region not found in kernel free list", &failures);
    }
    else
    {
        passes++;
    }

    const size_t kernel_used_after_free = vmm_test_count_regions(dispatcher.cpus[arch_cpu_id()].address_space->kernel_used_regions);
    if (kernel_used_after_free != kernel_used_before)
    {
        vmm_test_log_fail("kernel used list count did not restore after free", &failures);
    }
    else
    {
        passes++;
    }

    const size_t kernel_free_after_free = vmm_test_count_regions(dispatcher.cpus[arch_cpu_id()].address_space->kernel_free_regions);
    if (kernel_free_after_free != kernel_free_before)
    {
        vmm_test_log_fail("kernel free list count did not restore after free", &failures);
    }
    else
    {
        passes++;
    }

    pmm_free(range_block_va);

    kprintf("Arx kernel: vmm_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
    if (failures == 0)
    {
        kprintf("Arx kernel: vmm_test RESULT=PASS\n");
        KDEBUG("vmm_test passed with %llu checks\n", (unsigned long long) passes);
    }
    else
    {
        kprintf("Arx kernel: vmm_test RESULT=FAIL\n");
        KDEBUG("vmm_test failed with %llu checks\n", (unsigned long long) failures);
    }
}

static void pmm_test(void)
{
    static pmm_test_alloc_t allocs[PMM_TEST_MAX_PTRS];
    static void*            exhaust_ptrs[PMM_TEST_MAX_PTRS];

    const size_t scenario_sizes[PMM_TEST_SCENARIOS] = {
            1, 64, 511, 4096, 4097, 8192, 12288, 65536, 131072, 262144,
    };

    size_t failures                = 0;
    size_t passes                  = 0;
    size_t baseline_free_pages     = 0;
    size_t baseline_used_pages     = 0;
    size_t scenario_expected_pages = 0;

    kprintf("Arx kernel: pmm_test start\n");

    if (dispatcher.cpus[arch_cpu_id()].numa_node->zone.buddy_metadata == NULL)
    {
        pmm_test_log_fail("zone metadata is null (did pmm_init run?)", &failures);
        kprintf("Arx kernel: pmm_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
        return;
    }

    if (dispatcher.cpus[arch_cpu_id()].numa_node->zone.total_pages == 0)
    {
        pmm_test_log_fail("zone total_pages is zero", &failures);
    }
    else
    {
        passes++;
    }

    if (!pmm_test_check_zone_accounting(&dispatcher.cpus[arch_cpu_id()].numa_node->zone))
    {
        pmm_test_log_fail("zone accounting invariant failed at start (free+used!=total)", &failures);
    }
    else
    {
        passes++;
    }

    baseline_free_pages = dispatcher.cpus[arch_cpu_id()].numa_node->zone.free_pages;
    baseline_used_pages = dispatcher.cpus[arch_cpu_id()].numa_node->zone.used_pages;

    if (pmm_alloc(0) != NULL)
    {
        pmm_test_log_fail("pmm_alloc(0) should return NULL", &failures);
    }
    else
    {
        passes++;
    }

    for (size_t i = 0; i < PMM_TEST_MAX_PTRS; i++)
    {
        allocs[i].ptr       = NULL;
        allocs[i].pa_start  = 0;
        allocs[i].pa_end    = 0;
        allocs[i].requested = 0;
        allocs[i].actual    = 0;
        exhaust_ptrs[i]     = NULL;
    }

    for (size_t i = 0; i < PMM_TEST_SCENARIOS; i++)
    {
        size_t req       = scenario_sizes[i];
        size_t pow2pages = pmm_test_round_up_pow2_pages(req);
        size_t actual    = pow2pages * PAGE_SIZE;

        void* ptr = pmm_alloc(req);
        if (ptr == NULL)
        {
            pmm_test_log_fail("scenario allocation returned NULL", &failures);
            continue;
        }

        if (!pmm_test_is_page_aligned(ptr))
        {
            pmm_test_log_fail("allocated pointer is not page aligned", &failures);
        }
        else
        {
            passes++;
        }

        uintptr_t pa = hhdm_to_pa((uintptr_t) ptr, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);

        allocs[i].ptr       = ptr;
        allocs[i].requested = req;
        allocs[i].actual    = actual;
        allocs[i].pa_start  = pa;
        allocs[i].pa_end    = pa + actual;
        scenario_expected_pages += pow2pages;

        ((volatile uint8_t*) ptr)[0]          = (uint8_t) (0xA0 + i);
        ((volatile uint8_t*) ptr)[actual - 1] = (uint8_t) (0x5A + i);
    }

    if (!pmm_test_check_zone_accounting(&dispatcher.cpus[arch_cpu_id()].numa_node->zone))
    {
        pmm_test_log_fail("zone accounting invariant failed after scenario allocations", &failures);
    }
    else if (dispatcher.cpus[arch_cpu_id()].numa_node->zone.used_pages < baseline_used_pages + scenario_expected_pages)
    {
        pmm_test_log_fail("zone used_pages smaller than expected after scenario allocations", &failures);
    }
    else if (dispatcher.cpus[arch_cpu_id()].numa_node->zone.free_pages > baseline_free_pages)
    {
        pmm_test_log_fail("zone free_pages unexpectedly increased after scenario allocations", &failures);
    }
    else
    {
        passes++;
    }

    for (size_t i = 0; i < PMM_TEST_SCENARIOS; i++)
    {
        if (allocs[i].ptr == NULL)
        {
            continue;
        }

        for (size_t j = i + 1; j < PMM_TEST_SCENARIOS; j++)
        {
            if (allocs[j].ptr == NULL)
            {
                continue;
            }

            bool overlap = !(allocs[i].pa_end <= allocs[j].pa_start || allocs[j].pa_end <= allocs[i].pa_start);
            if (overlap)
            {
                pmm_test_log_fail("allocation ranges overlap", &failures);
            }
        }
    }
    passes++;

    for (size_t i = 0; i < PMM_TEST_SCENARIOS; i++)
    {
        if (allocs[i].ptr != NULL)
        {
            pmm_free(allocs[i].ptr);
            allocs[i].ptr = NULL;
        }
    }

    if (!pmm_test_check_zone_accounting(&dispatcher.cpus[arch_cpu_id()].numa_node->zone))
    {
        pmm_test_log_fail("zone accounting invariant failed after scenario frees", &failures);
    }
    else if (dispatcher.cpus[arch_cpu_id()].numa_node->zone.free_pages != baseline_free_pages || dispatcher.cpus[arch_cpu_id()].numa_node->zone.used_pages != baseline_used_pages)
    {
        pmm_test_log_fail("zone free/used pages did not return to baseline after scenario frees", &failures);
    }
    else
    {
        passes++;
    }

    passes++;

    for (size_t round = 0; round < 64; round++)
    {
        size_t n = 0;
        while (n < PMM_TEST_MAX_PTRS)
        {
            void* ptr = pmm_alloc(PAGE_SIZE);
            if (ptr == NULL)
            {
                break;
            }
            exhaust_ptrs[n++] = ptr;
        }

        if (n == 0)
        {
            pmm_test_log_fail("stress round could not allocate even one page", &failures);
            break;
        }

        for (size_t i = 0; i < n; i++)
        {
            pmm_free(exhaust_ptrs[i]);
            exhaust_ptrs[i] = NULL;
        }

        if (!pmm_test_check_zone_accounting(&dispatcher.cpus[arch_cpu_id()].numa_node->zone))
        {
            pmm_test_log_fail("zone accounting invariant failed during stress round", &failures);
            break;
        }

        if (dispatcher.cpus[arch_cpu_id()].numa_node->zone.free_pages != baseline_free_pages || dispatcher.cpus[arch_cpu_id()].numa_node->zone.used_pages != baseline_used_pages)
        {
            pmm_test_log_fail("zone free/used pages drifted during stress round", &failures);
            break;
        }
    }
    passes++;

    size_t exhausted_count = 0;
    while (exhausted_count < PMM_TEST_MAX_PTRS)
    {
        void* ptr = pmm_alloc(PAGE_SIZE);
        if (ptr == NULL)
        {
            break;
        }
        exhaust_ptrs[exhausted_count++] = ptr;
    }

    if (exhausted_count == 0)
    {
        pmm_test_log_fail("exhaustion probe allocated zero pages", &failures);
    }
    else
    {
        passes++;
    }

    for (size_t i = 0; i < exhausted_count; i++)
    {
        pmm_free(exhaust_ptrs[i]);
        exhaust_ptrs[i] = NULL;
    }

    if (!pmm_test_check_zone_accounting(&dispatcher.cpus[arch_cpu_id()].numa_node->zone))
    {
        pmm_test_log_fail("zone accounting invariant failed after exhaustion cleanup", &failures);
    }
    else if (dispatcher.cpus[arch_cpu_id()].numa_node->zone.free_pages != baseline_free_pages || dispatcher.cpus[arch_cpu_id()].numa_node->zone.used_pages != baseline_used_pages)
    {
        pmm_test_log_fail("zone free/used pages not restored after exhaustion cleanup", &failures);
    }
    else
    {
        passes++;
    }

    passes++;

    kprintf("Arx kernel: pmm_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
    if (failures == 0)
    {
        kprintf("Arx kernel: pmm_test RESULT=PASS\n");
        KDEBUG("pmm_test passed with %llu checks\n", (unsigned long long) passes);
    }
    else
    {
        kprintf("Arx kernel: pmm_test RESULT=FAIL\n");
        KDEBUG("pmm_test failed with %llu checks\n", (unsigned long long) failures);
    }
}

static void heap_test_log_fail(const char* message, size_t* failures)
{
    (*failures)++;
    kprintf("Arx kernel: heap_test FAIL: %s\n", message);
}

static void heap_test(void)
{
    size_t failures = 0;
    size_t passes   = 0;

    cpu_info_t* cpu = &dispatcher.cpus[arch_cpu_id()];
    if (cpu->numa_node == NULL)
    {
        heap_test_log_fail("cpu numa_node is NULL", &failures);
        kprintf("Arx kernel: heap_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
        kprintf("Arx kernel: heap_test RESULT=FAIL\n");
        return;
    }

    kernel_heap_t* heap = &cpu->numa_node->heap;

    kprintf("Arx kernel: heap_test start\n");

    if (heap_alloc(heap, 0) != NULL)
    {
        heap_test_log_fail("heap_alloc(0) should return NULL", &failures);
    }
    else
    {
        passes++;
    }

    void* p16 = heap_alloc(heap, 16);
    void* p32 = heap_alloc(heap, 32);
    void* p64 = heap_alloc(heap, 64);

    if (p16 == NULL || p32 == NULL || p64 == NULL)
    {
        heap_test_log_fail("basic class allocations returned NULL", &failures);
    }
    else
    {
        passes++;
    }

    if (p16 == p32 || p16 == p64 || p32 == p64)
    {
        heap_test_log_fail("distinct allocations returned duplicate pointers", &failures);
    }
    else
    {
        passes++;
    }

    heap_free(heap, p16);
    heap_free(heap, p32);
    heap_free(heap, p64);
    passes++;

    const size_t odd_sizes[] = {17, 33, 65, 129, 257, 513, 777, 1500, 2047};
    void*        odd_ptrs[sizeof(odd_sizes) / sizeof(odd_sizes[0])];

    for (size_t i = 0; i < (sizeof(odd_ptrs) / sizeof(odd_ptrs[0])); i++)
    {
        odd_ptrs[i] = heap_alloc(heap, odd_sizes[i]);
        if (odd_ptrs[i] == NULL)
        {
            heap_test_log_fail("odd-size allocation returned NULL", &failures);
        }
    }

    bool odd_unique = true;
    for (size_t i = 0; i < (sizeof(odd_ptrs) / sizeof(odd_ptrs[0])); i++)
    {
        for (size_t j = i + 1; j < (sizeof(odd_ptrs) / sizeof(odd_ptrs[0])); j++)
        {
            if (odd_ptrs[i] != NULL && odd_ptrs[i] == odd_ptrs[j])
            {
                odd_unique = false;
            }
        }
    }

    if (!odd_unique)
    {
        heap_test_log_fail("odd-size allocations returned duplicate pointers", &failures);
    }
    else
    {
        passes++;
    }

    if (heap_alloc(heap, 2049) != NULL)
    {
        heap_test_log_fail("oversized heap_alloc(2049) should return NULL", &failures);
    }
    else
    {
        passes++;
    }

    for (size_t i = 0; i < (sizeof(odd_ptrs) / sizeof(odd_ptrs[0])); i++)
    {
        heap_free(heap, odd_ptrs[i]);
    }
    passes++;

    void* reuse_a = heap_alloc(heap, 32);
    if (reuse_a == NULL)
    {
        heap_test_log_fail("reuse probe first alloc failed", &failures);
    }
    else
    {
        void* reuse_b = heap_alloc(heap, 32);
        if (reuse_b == NULL)
        {
            heap_test_log_fail("reuse probe second alloc failed", &failures);
        }
        else
        {
            heap_free(heap, reuse_a);
            void* reuse_c = heap_alloc(heap, 32);

            if (reuse_c != reuse_a)
            {
                heap_test_log_fail("freed object was not reused", &failures);
            }
            else
            {
                passes++;
            }

            heap_free(heap, reuse_b);
            heap_free(heap, reuse_c);
        }
    }

    void* stress_ptrs[300];
    size_t stress_count = 0;

    for (size_t i = 0; i < 300; i++)
    {
        stress_ptrs[i] = heap_alloc(heap, 64);
        if (stress_ptrs[i] == NULL)
        {
            break;
        }
        stress_count++;
    }

    if (stress_count < 257)
    {
        heap_test_log_fail("cache growth probe did not allocate beyond initial slab set", &failures);
    }
    else
    {
        passes++;
    }

    for (size_t i = 0; i < stress_count; i++)
    {
        heap_free(heap, stress_ptrs[i]);
    }
    passes++;

    kprintf("Arx kernel: heap_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
    if (failures == 0)
    {
        kprintf("Arx kernel: heap_test RESULT=PASS\n");
        KDEBUG("heap_test passed with %llu checks\n", (unsigned long long) passes);
    }
    else
    {
        kprintf("Arx kernel: heap_test RESULT=FAIL\n");
        KDEBUG("heap_test failed with %llu checks\n", (unsigned long long) failures);
    }
}

void run_memory_selftests(void)
{
    pmm_test();
    vmm_test();
    heap_test();
}
