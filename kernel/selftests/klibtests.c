// Ai generated testing
// Not thread safe

#include <klib/klib.h>
#include <memory/vmm.h>
#include <selftests/selftests.h>

static void klib_test_log_fail(const char* message, size_t* failures)
{
    (*failures)++;
    kprintf("Arx kernel: klib_test FAIL: %s\n", message);
}

static void vmalloc_test(size_t* passes, size_t* failures)
{
    const size_t req_size     = PAGE_SIZE * 100 + 123;
    const size_t aligned_size = align_up(req_size, PAGE_SIZE);

    void* ptr = vmalloc(req_size);
    if (ptr == NULL)
    {
        klib_test_log_fail("vmalloc returned NULL", failures);
        return;
    }
    (*passes)++;

    memset(ptr, 0, req_size);

    if ((((uintptr_t) ptr) & (PAGE_SIZE - 1)) != 0)
    {
        klib_test_log_fail("vmalloc returned non-page-aligned pointer", failures);
    }
    else
    {
        (*passes)++;
    }

    virt_region_t* region = vmm_find_region(dispatcher.cpus[arch_cpu_id()].address_space, (virt_addr_t) (uintptr_t) ptr);
    if (region == NULL)
    {
        klib_test_log_fail("allocated vmalloc region not found in VMM used regions", failures);
    }
    else if (region->start != (virt_addr_t) (uintptr_t) ptr || region->size != aligned_size)
    {
        klib_test_log_fail("vmalloc region metadata mismatch", failures);
    }
    else
    {
        (*passes)++;
    }

    if (vmm_virt_to_phys((virt_addr_t) (uintptr_t) ptr, dispatcher.cpus[arch_cpu_id()].address_space) == 0)
    {
        klib_test_log_fail("allocated vmalloc address is not mapped", failures);
    }
    else
    {
        (*passes)++;
    }

    vfree(ptr);

    if (vmm_find_region(dispatcher.cpus[arch_cpu_id()].address_space, (virt_addr_t) (uintptr_t) ptr) != NULL)
    {
        klib_test_log_fail("vmalloc region still present after vfree", failures);
    }
    else
    {
        (*passes)++;
    }

    if (vmm_virt_to_phys((virt_addr_t) (uintptr_t) ptr, dispatcher.cpus[arch_cpu_id()].address_space) != 0)
    {
        klib_test_log_fail("vmalloc mapping still present after vfree", failures);
    }
    else
    {
        (*passes)++;
    }
}

static void kmalloc_test(size_t* passes, size_t* failures)
{
    if (kmalloc(0) != NULL)
    {
        klib_test_log_fail("kmalloc(0) should return NULL", failures);
    }
    else
    {
        (*passes)++;
    }

    const size_t sizes[] = {1, 17, 33, 65, 129, 513, 777, 1500, 2047};
    void*        ptrs[sizeof(sizes) / sizeof(sizes[0])];

    for (size_t i = 0; i < (sizeof(ptrs) / sizeof(ptrs[0])); i++)
    {
        ptrs[i] = kmalloc(sizes[i]);
        if (ptrs[i] == NULL)
        {
            klib_test_log_fail("kmalloc odd-size allocation returned NULL", failures);
            continue;
        }

        ((volatile uint8_t*) ptrs[i])[0] = (uint8_t) (0xA0 + i);
        (*passes)++;
    }

    bool unique = true;
    for (size_t i = 0; i < (sizeof(ptrs) / sizeof(ptrs[0])); i++)
    {
        for (size_t j = i + 1; j < (sizeof(ptrs) / sizeof(ptrs[0])); j++)
        {
            if (ptrs[i] != NULL && ptrs[i] == ptrs[j])
            {
                unique = false;
            }
        }
    }

    if (!unique)
    {
        klib_test_log_fail("kmalloc returned duplicate pointers for active allocations", failures);
    }
    else
    {
        (*passes)++;
    }

    if (kmalloc(131073) != NULL)
    {
        klib_test_log_fail("kmalloc(131073) should return NULL", failures);
    }
    else
    {
        (*passes)++;
    }

    for (size_t i = 0; i < (sizeof(ptrs) / sizeof(ptrs[0])); i++)
    {
        kfree(ptrs[i]);
    }
    (*passes)++;

    void* r1 = kmalloc(64);
    void* r2 = kmalloc(64);
    if (r1 == NULL || r2 == NULL)
    {
        klib_test_log_fail("kmalloc reuse probe allocations failed", failures);
    }
    else
    {
        kfree(r1);
        void* r3 = kmalloc(64);
        if (r3 == NULL)
        {
            klib_test_log_fail("kmalloc reuse probe re-allocation failed", failures);
        }
        else
        {
            (*passes)++;
            kfree(r3);
        }

        kfree(r2);
    }
}

static void kzalloc_test(size_t* passes, size_t* failures)
{
    if (kzalloc(0) != NULL)
    {
        klib_test_log_fail("kzalloc(0) should return NULL", failures);
    }
    else
    {
        (*passes)++;
    }

    const size_t sizes[] = {1, 8, 17, 64, 129, 777, 1500, 2047};
    void*        ptrs[sizeof(sizes) / sizeof(sizes[0])];

    for (size_t i = 0; i < (sizeof(ptrs) / sizeof(ptrs[0])); i++)
    {
        ptrs[i] = kzalloc(sizes[i]);
        if (ptrs[i] == NULL)
        {
            klib_test_log_fail("kzalloc allocation returned NULL", failures);
            continue;
        }

        bool all_zero = true;
        for (size_t j = 0; j < sizes[i]; j++)
        {
            if (((uint8_t*) ptrs[i])[j] != 0)
            {
                all_zero = false;
                break;
            }
        }

        if (!all_zero)
        {
            klib_test_log_fail("kzalloc memory not zero-initialized", failures);
        }
        else
        {
            (*passes)++;
        }
    }

    if (kzalloc(131073) != NULL)
    {
        klib_test_log_fail("kzalloc(131073) should return NULL", failures);
    }
    else
    {
        (*passes)++;
    }

    for (size_t i = 0; i < (sizeof(ptrs) / sizeof(ptrs[0])); i++)
    {
        kfree(ptrs[i]);
    }
    (*passes)++;
}

void run_klib_selftests(void)
{
    size_t failures = 0;
    size_t passes   = 0;

    kprintf("Arx kernel: klib_test start\n");

    vmalloc_test(&passes, &failures);
    kmalloc_test(&passes, &failures);
    kzalloc_test(&passes, &failures);

    kprintf("Arx kernel: klib_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
    if (failures == 0)
    {
        kprintf("Arx kernel: klib_test RESULT=PASS\n");
        KDEBUG("klib_test passed with %llu checks\n", (unsigned long long) passes);
    }
    else
    {
        kprintf("Arx kernel: klib_test RESULT=FAIL\n");
        KDEBUG("klib_test failed with %llu checks\n", (unsigned long long) failures);
    }
}
