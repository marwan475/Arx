// Ai generated testing

#include <memory/pmm.h>

#define PMM_TEST_MAX_PTRS 2048
#define PMM_TEST_SCENARIOS 10

typedef struct pmm_test_alloc
{
    void*    ptr;
    uintptr_t pa_start;
    uintptr_t pa_end;
    size_t   requested;
    size_t   actual;
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

void pmm_test(void)
{
    static pmm_test_alloc_t allocs[PMM_TEST_MAX_PTRS];
    static void*            exhaust_ptrs[PMM_TEST_MAX_PTRS];

    const size_t scenario_sizes[PMM_TEST_SCENARIOS] = {
        1,
        64,
        511,
        4096,
        4097,
        8192,
        12288,
        65536,
        131072,
        262144,
    };

    size_t failures = 0;
    size_t passes   = 0;

    kprintf("Arx kernel: pmm_test start\n");

    if (pmm_zone.buddy_metadata == NULL)
    {
        pmm_test_log_fail("zone metadata is null (did pmm_init run?)", &failures);
        kprintf("Arx kernel: pmm_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
        return;
    }

    if (pmm_zone.total_pages == 0)
    {
        pmm_test_log_fail("zone total_pages is zero", &failures);
    }
    else
    {
        passes++;
        kprintf("Arx kernel: pmm_test pass: zone initialized (pages=%llu, regions=%llu)\n", (unsigned long long) pmm_zone.total_pages, (unsigned long long) pmm_zone.region_count);
    }

    if (pmm_alloc(&pmm_zone, 0) != NULL)
    {
        pmm_test_log_fail("pmm_alloc(zone, 0) should return NULL", &failures);
    }
    else
    {
        passes++;
        kprintf("Arx kernel: pmm_test pass: zero-size alloc rejected\n");
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

        void* ptr = pmm_alloc(&pmm_zone, req);
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

        uintptr_t pa = hhdm_to_pa((uintptr_t) ptr, pmm_zone.hhdm_present, pmm_zone.hhdm_offset);

        allocs[i].ptr       = ptr;
        allocs[i].requested = req;
        allocs[i].actual    = actual;
        allocs[i].pa_start  = pa;
        allocs[i].pa_end    = pa + actual;

        // Touch edges to catch obvious bad mappings and accidental aliasing.
        ((volatile uint8_t*) ptr)[0]          = (uint8_t) (0xA0 + i);
        ((volatile uint8_t*) ptr)[actual - 1] = (uint8_t) (0x5A + i);
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
    kprintf("Arx kernel: pmm_test pass: overlap check complete\n");

    for (size_t i = 0; i < PMM_TEST_SCENARIOS; i++)
    {
        if (allocs[i].ptr != NULL)
        {
            pmm_free(&pmm_zone, allocs[i].ptr);
            allocs[i].ptr = NULL;
        }
    }
    passes++;
    kprintf("Arx kernel: pmm_test pass: scenario blocks freed\n");

    // Fragmentation and merge pressure: repeated small alloc/free bursts.
    for (size_t round = 0; round < 64; round++)
    {
        size_t n = 0;
        while (n < PMM_TEST_MAX_PTRS)
        {
            void* ptr = pmm_alloc(&pmm_zone, PAGE_SIZE);
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
            pmm_free(&pmm_zone, exhaust_ptrs[i]);
            exhaust_ptrs[i] = NULL;
        }
    }
    passes++;
    kprintf("Arx kernel: pmm_test pass: stress rounds complete\n");

    // Basic exhaustion behavior check.
    size_t exhausted_count = 0;
    while (exhausted_count < PMM_TEST_MAX_PTRS)
    {
        void* ptr = pmm_alloc(&pmm_zone, PAGE_SIZE);
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
        kprintf("Arx kernel: pmm_test pass: exhaustion probe allocated %llu pages\n", (unsigned long long) exhausted_count);
    }

    for (size_t i = 0; i < exhausted_count; i++)
    {
        pmm_free(&pmm_zone, exhaust_ptrs[i]);
        exhaust_ptrs[i] = NULL;
    }
    passes++;
    kprintf("Arx kernel: pmm_test pass: exhaustion probe cleanup complete\n");

    kprintf("Arx kernel: pmm_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
    if (failures == 0)
    {
        kprintf("Arx kernel: pmm_test RESULT=PASS\n");
    }
    else
    {
        kprintf("Arx kernel: pmm_test RESULT=FAIL\n");
    }
}

void run_selftests(void)
{
    pmm_test();
}