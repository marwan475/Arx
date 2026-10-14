// Ai generated testing
// Not thread safe

#include <klib/klib.h>
#include <memory/vmm.h>
#include <platform.h>
#include <selftests/selftests.h>

static void klib_test_log_fail(const char* message, size_t* failures)
{
    (*failures)++;
    selftest_record_failure_detail(message);
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

    virt_region_t* region = vmm_find_region(platform.cpus[arch_cpu_id()].address_space, (virt_addr_t) (uintptr_t) ptr);
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

    if (vmm_virt_to_phys((virt_addr_t) (uintptr_t) ptr, platform.cpus[arch_cpu_id()].address_space) == 0)
    {
        klib_test_log_fail("allocated vmalloc address is not mapped", failures);
    }
    else
    {
        (*passes)++;
    }

    vfree(ptr);

    if (vmm_find_region(platform.cpus[arch_cpu_id()].address_space, (virt_addr_t) (uintptr_t) ptr) != NULL)
    {
        klib_test_log_fail("vmalloc region still present after vfree", failures);
    }
    else
    {
        (*passes)++;
    }

    if (vmm_virt_to_phys((virt_addr_t) (uintptr_t) ptr, platform.cpus[arch_cpu_id()].address_space) != 0)
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

static void sort_u32_test(size_t* passes, size_t* failures)
{
    uint32_t       values[]   = {UINT32_MAX, 4, 0, 4, 1, UINT32_MAX - 1, 2};
    const uint32_t expected[] = {0, 1, 2, 4, 4, UINT32_MAX - 1, UINT32_MAX};
    sort_u32(NULL, 0);
    sort_u32(values, 0);
    sort_u32(values, 1);
    if (values[0] != UINT32_MAX)
        klib_test_log_fail("sort_u32 empty/single element changed data", failures);
    else
        (*passes)++;
    sort_u32(values, sizeof(values) / sizeof(values[0]));
    if (memcmp(values, expected, sizeof(values)) != 0)
        klib_test_log_fail("sort_u32 ordering, duplicates and uint32 bounds", failures);
    else
        (*passes)++;
    sort_u32(values, sizeof(values) / sizeof(values[0]));
    if (memcmp(values, expected, sizeof(values)) != 0)
        klib_test_log_fail("sort_u32 sorted input", failures);
    else
        (*passes)++;
}

static void error_logger_test(size_t* passes, size_t* failures)
{
    // An isolated buffer avoids clearing actual boot errors or printing fake
    // failures in the end-of-init dump. Static storage avoids a large stack.
    static error_log_buffer_t buffer;
    char output[ERROR_LOGGER_MESSAGE_SIZE];
    memset(&buffer, 0, sizeof(buffer));
    if (error_logger_read(&buffer, 0, output, sizeof(output))
        || error_logger_append(NULL, "bad") || error_logger_append(&buffer, NULL)
        || error_logger_append(&buffer, "") || buffer.reserved != 0)
        klib_test_log_fail("error logger empty/invalid inputs", failures);
    else (*passes)++;

    if (!error_logger_append(&buffer, "clock %s status=%u address=0x%llx", "HPET", 3U, 0x123456789abcdef0ULL)
        || !error_logger_read(&buffer, 0, output, sizeof(output))
        || strcmp(output, "clock HPET status=3 address=0x123456789abcdef0") != 0)
        klib_test_log_fail("error logger formatted diagnostic", failures);
    else (*passes)++;

    char small[5] = {'?', '?', '?', '?', '!'};
    if (!error_logger_read(&buffer, 0, small, 4) || strcmp(small, "clo") != 0 || small[4] != '!'
        || error_logger_read(&buffer, 0, output, 0) || error_logger_read(NULL, 0, output, sizeof(output))
        || error_logger_read(&buffer, 0, NULL, sizeof(output))
        || error_logger_read(&buffer, ERROR_LOGGER_CAPACITY, output, sizeof(output)))
        klib_test_log_fail("error logger bounded reads", failures);
    else (*passes)++;

    char longMessage[ERROR_LOGGER_MESSAGE_SIZE + 32];
    memset(longMessage, 'x', sizeof(longMessage) - 1);
    longMessage[sizeof(longMessage) - 1] = '\0';
    if (!error_logger_append(&buffer, "%s", longMessage)
        || !error_logger_read(&buffer, 1, output, sizeof(output))
        || strlen(output) != ERROR_LOGGER_MESSAGE_SIZE - 1
        || strcmp(output + ERROR_LOGGER_MESSAGE_SIZE - sizeof(" [truncated]"), " [truncated]") != 0)
        klib_test_log_fail("error logger marks truncated diagnostics", failures);
    else (*passes)++;

    bool filled = true;
    for (size_t i = 2; i < ERROR_LOGGER_CAPACITY; ++i)
        filled &= error_logger_append(&buffer, "entry %u", (unsigned) i);
    if (!filled || error_logger_append(&buffer, "overflow")
        || buffer.reserved != ERROR_LOGGER_CAPACITY + 1
        || !error_logger_read(&buffer, 0, output, sizeof(output))
        || strcmp(output, "clock HPET status=3 address=0x123456789abcdef0") != 0
        || !error_logger_read(&buffer, ERROR_LOGGER_CAPACITY - 1, output, sizeof(output)))
        klib_test_log_fail("error logger full buffer preserves earliest diagnostics", failures);
    else (*passes)++;

    // A slot reserved by another CPU must remain unreadable until publication.
    memset(&buffer, 0, sizeof(buffer));
    buffer.reserved = 1;
    memcpy(buffer.entries[0].message, "unfinished", sizeof("unfinished"));
    if (error_logger_read(&buffer, 0, output, sizeof(output)))
        klib_test_log_fail("error logger does not read unpublished slots", failures);
    else (*passes)++;
}

void run_klib_selftests(void)
{
    size_t failures = 0;
    size_t passes   = 0;

    selftest_case_begin("klib_test");

    vmalloc_test(&passes, &failures);
    kmalloc_test(&passes, &failures);
    kzalloc_test(&passes, &failures);
    sort_u32_test(&passes, &failures);
    error_logger_test(&passes, &failures);

    selftest_case_end("klib_test", (unsigned long long) passes, (unsigned long long) failures);
}
