// Ai generated testing
// Not thread safe

#include <klib/bitmap.h>
#include <klib/intrusive_list.h>
#include <klib/klib.h>
#include <selftests/selftests.h>

typedef struct ilist_test_node
{
    struct ilist_test_node* next;
    struct ilist_test_node* prev;
    int                     value;
} ilist_test_node_t;

static void ilist_test_log_fail(const char* message, size_t* failures)
{
    (*failures)++;
    kprintf("Arx kernel: ilist_test FAIL: %s\n", message);
}

static void bitmap_selftest_log_fail(const char* message, size_t* failures)
{
    (*failures)++;
    kprintf("Arx kernel: bitmap_selftest FAIL: %s\n", message);
}

static void bitmap_selftest(void)
{
    size_t failures = 0;
    size_t passes   = 0;

    enum
    {
        BIT_COUNT = 64,
    };

    uint8_t bitmap[BITMAP_BYTES_FOR_BITS(BIT_COUNT)];

    kprintf("Arx kernel: bitmap_selftest start\n");

    bitmap_init(bitmap, BIT_COUNT);
    bool all_clear = true;
    for (size_t i = 0; i < BIT_COUNT; i++)
    {
        if (bitmap_test(bitmap, i))
        {
            all_clear = false;
            break;
        }
    }
    if (!all_clear)
    {
        bitmap_selftest_log_fail("bitmap_init did not clear all bits", &failures);
    }
    else
    {
        passes++;
    }

    bitmap_set(bitmap, 3);
    bitmap_set(bitmap, 17);
    if (!bitmap_test(bitmap, 3) || !bitmap_test(bitmap, 17) || bitmap_test(bitmap, 4))
    {
        bitmap_selftest_log_fail("bitmap_set/bitmap_test basic behavior mismatch", &failures);
    }
    else
    {
        passes++;
    }

    bitmap_clear(bitmap, 3);
    if (bitmap_test(bitmap, 3) || !bitmap_test(bitmap, 17))
    {
        bitmap_selftest_log_fail("bitmap_clear did not clear selected bit", &failures);
    }
    else
    {
        passes++;
    }

    bitmap_set_range(bitmap, 24, 5); // [24..28]
    bool range_set_ok = true;
    for (size_t i = 24; i <= 28; i++)
    {
        if (!bitmap_test(bitmap, i))
        {
            range_set_ok = false;
            break;
        }
    }
    if (!range_set_ok)
    {
        bitmap_selftest_log_fail("bitmap_set_range failed", &failures);
    }
    else
    {
        passes++;
    }

    bitmap_clear_range(bitmap, 25, 3); // clear [25..27]
    if (!bitmap_test(bitmap, 24) || bitmap_test(bitmap, 25) || bitmap_test(bitmap, 26) || bitmap_test(bitmap, 27) || !bitmap_test(bitmap, 28))
    {
        bitmap_selftest_log_fail("bitmap_clear_range failed", &failures);
    }
    else
    {
        passes++;
    }

    size_t index = 0;
    bitmap_init(bitmap, BIT_COUNT);
    bitmap_set_range(bitmap, 0, 6);
    if (!bitmap_find_first_clear(bitmap, BIT_COUNT, &index) || index != 6)
    {
        bitmap_selftest_log_fail("bitmap_find_first_clear returned wrong index", &failures);
    }
    else
    {
        passes++;
    }

    bitmap_init(bitmap, BIT_COUNT);
    bitmap_set(bitmap, 11);
    bitmap_set(bitmap, 19);
    if (!bitmap_find_first_set(bitmap, BIT_COUNT, &index) || index != 11)
    {
        bitmap_selftest_log_fail("bitmap_find_first_set returned wrong index", &failures);
    }
    else
    {
        passes++;
    }

    bitmap_set_range(bitmap, 0, BIT_COUNT);
    if (bitmap_find_first_clear(bitmap, BIT_COUNT, &index))
    {
        bitmap_selftest_log_fail("bitmap_find_first_clear should fail when full", &failures);
    }
    else
    {
        passes++;
    }

    bitmap_init(bitmap, BIT_COUNT);
    if (bitmap_find_first_set(bitmap, BIT_COUNT, &index))
    {
        bitmap_selftest_log_fail("bitmap_find_first_set should fail when empty", &failures);
    }
    else
    {
        passes++;
    }

    kprintf("Arx kernel: bitmap_selftest summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
    if (failures == 0)
    {
        kprintf("Arx kernel: bitmap_selftest RESULT=PASS\n");
        KDEBUG("bitmap_selftest passed with %llu checks\n", (unsigned long long) passes);
    }
    else
    {
        kprintf("Arx kernel: bitmap_selftest RESULT=FAIL\n");
        KDEBUG("bitmap_selftest failed with %llu checks\n", (unsigned long long) failures);
    }
}

static void ilist_test(void)
{
    size_t failures = 0;
    size_t passes   = 0;

    ilist_test_node_t n1 = {0};
    ilist_test_node_t n2 = {0};
    ilist_test_node_t n3 = {0};
    ilist_test_node_t n4 = {0};
    ilist_test_node_t n5 = {0};

    n1.value = 1;
    n2.value = 2;
    n3.value = 3;
    n4.value = 4;
    n5.value = 5;

    ilist_test_node_t* head = NULL;

    kprintf("Arx kernel: ilist_test start\n");

    ILIST_NODE_INIT(&n1);
    ILIST_NODE_INIT(&n2);
    ILIST_NODE_INIT(&n3);
    ILIST_NODE_INIT(&n4);
    ILIST_NODE_INIT(&n5);

    if (n1.next != NULL || n1.prev != NULL || n2.next != NULL || n2.prev != NULL || n3.next != NULL || n3.prev != NULL || n4.next != NULL || n4.prev != NULL || n5.next != NULL || n5.prev != NULL)
    {
        ilist_test_log_fail("ILIST_NODE_INIT did not clear links", &failures);
    }
    else
    {
        passes++;
    }

    ILIST_PUSH_FRONT(head, &n1); // [1]
    ILIST_PUSH_FRONT(head, &n2); // [2,1]
    ILIST_APPEND(head, &n3);     // [2,1,3]
    ILIST_INSERT_BEFORE(head, &n3, &n4); // [2,1,4,3]

    if (head != &n2 || n2.next != &n1 || n1.prev != &n2 || n1.next != &n4 || n4.prev != &n1 || n4.next != &n3 || n3.prev != &n4 || n3.next != NULL)
    {
        ilist_test_log_fail("insert/link structure mismatch", &failures);
    }
    else
    {
        passes++;
    }

    size_t count = 0;
    int    sum   = 0;
    ILIST_FOR_EACH(it, head)
    {
        count++;
        sum += it->value;
    }

    if (count != 4 || sum != (2 + 1 + 4 + 3))
    {
        ilist_test_log_fail("ILIST_FOR_EACH traversal mismatch", &failures);
    }
    else
    {
        passes++;
    }

    ILIST_REMOVE(head, &n1); // [2,4,3]
    if (head != &n2 || n2.next != &n4 || n4.prev != &n2 || n1.next != NULL || n1.prev != NULL)
    {
        ilist_test_log_fail("remove middle node failed", &failures);
    }
    else
    {
        passes++;
    }

    ILIST_REMOVE(head, &n2); // [4,3]
    if (head != &n4 || n4.prev != NULL || n2.next != NULL || n2.prev != NULL)
    {
        ilist_test_log_fail("remove head node failed", &failures);
    }
    else
    {
        passes++;
    }

    ILIST_APPEND(head, &n5); // [4,3,5]
    if (n5.prev != &n3 || n5.next != NULL || n3.next != &n5)
    {
        ilist_test_log_fail("append tail node failed", &failures);
    }
    else
    {
        passes++;
    }

    ILIST_FOR_EACH_SAFE(it, next_it, head)
    {
        ILIST_REMOVE(head, it);
    }

    if (head != NULL || n3.next != NULL || n3.prev != NULL || n4.next != NULL || n4.prev != NULL || n5.next != NULL || n5.prev != NULL)
    {
        ilist_test_log_fail("safe remove-all traversal failed", &failures);
    }
    else
    {
        passes++;
    }

    kprintf("Arx kernel: ilist_test summary: pass=%llu fail=%llu\n", (unsigned long long) passes, (unsigned long long) failures);
    if (failures == 0)
    {
        kprintf("Arx kernel: ilist_test RESULT=PASS\n");
        KDEBUG("ilist_test passed with %llu checks\n", (unsigned long long) passes);
    }
    else
    {
        kprintf("Arx kernel: ilist_test RESULT=FAIL\n");
        KDEBUG("ilist_test failed with %llu checks\n", (unsigned long long) failures);
    }
}

void run_datastructures_selftests(void)
{
    bitmap_selftest();
    ilist_test();
}
