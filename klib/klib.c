#include <klib/klib.h>
#include <klib/printf/printf.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <platform.h>

spinlock_t kprintf_lock = 0;

uint64_t extend_counter32(uint64_t previous, uint32_t sample)
{
    return previous + (uint32_t) (sample - (uint32_t) previous);
}

static error_log_buffer_t boot_errors;

static void error_logger_format(char* message, const char* format, va_list args)
{
    const int length = vsnprintf_(message, ERROR_LOGGER_MESSAGE_SIZE, format, args);

    if (length < 0)
    {
        memcpy(message, "error formatting diagnostic", sizeof("error formatting diagnostic"));
    }
    else if ((size_t) length >= ERROR_LOGGER_MESSAGE_SIZE)
        memcpy(message + ERROR_LOGGER_MESSAGE_SIZE - sizeof(" [truncated]"), " [truncated]", sizeof(" [truncated]"));
}

static bool error_logger_publish(error_log_buffer_t* buffer, const char* message)
{
    if (message[0] == '\0')
    {
        return false;
    }

    // Each writer owns one slot. An interrupt/NMI cannot deadlock on a logger
    // lock held by its interrupted context; ready publishes the completed copy.
    const uint64_t index = __atomic_fetch_add(&buffer->reserved, 1ULL, __ATOMIC_RELAXED);

    if (index >= ERROR_LOGGER_CAPACITY)
    {
        return false;
    }

    memcpy(buffer->entries[index].message, message, strlen(message) + 1);
    __atomic_store_n(&buffer->entries[index].ready, 1, __ATOMIC_RELEASE);
    return true;
}

bool error_logger_append(error_log_buffer_t* buffer, const char* format, ...)
{
    if (buffer == NULL || format == NULL)
    {
        return false;
    }

    char    message[ERROR_LOGGER_MESSAGE_SIZE];
    va_list args;
    va_start(args, format);
    error_logger_format(message, format, args);
    va_end(args);
    return error_logger_publish(buffer, message);
}

bool error_logger_read(const error_log_buffer_t* buffer, size_t index, char* message, size_t capacity)
{
    if (buffer == NULL || message == NULL || capacity == 0 || index >= ERROR_LOGGER_CAPACITY)
    {
        return false;
    }

    if (!__atomic_load_n(&buffer->entries[index].ready, __ATOMIC_ACQUIRE))
    {
        return false;
    }

    const char* source = buffer->entries[index].message;
    size_t      length = strlen(source);

    if (length >= capacity)
    {
        length = capacity - 1;
    }

    memcpy(message, source, length);
    message[length] = '\0';
    return true;
}

void error_logger(const char* format, ...)
{
    if (format == NULL)
    {
        return;
    }

    char    message[ERROR_LOGGER_MESSAGE_SIZE];
    va_list args;
    va_start(args, format);
    error_logger_format(message, format, args);
    va_end(args);
    (void) error_logger_publish(&boot_errors, message);
    // Store first: even an emergency serial failure cannot discard the record.
    kprintf("%s%s", message, message[0] != '\0' && message[strlen(message) - 1] != '\n' ? "\n" : "");
}

static void error_logger_output(const char* message)
{
    KDEBUG("%s", message);
    kprintf("%s", message);
}

void error_logger_print(void)
{
    const uint64_t reserved = __atomic_load_n(&boot_errors.reserved, __ATOMIC_ACQUIRE);
    const uint64_t count    = reserved < ERROR_LOGGER_CAPACITY ? reserved : ERROR_LOGGER_CAPACITY;
    char           message[ERROR_LOGGER_MESSAGE_SIZE];
    char           line[ERROR_LOGGER_MESSAGE_SIZE + 96];
    error_logger_output("\n========== Kernel error log ==========\n");

    if (count == 0)
    {
        error_logger_output("No recorded kernel errors.\n");
    }

    uint64_t pending = 0;

    for (uint64_t i = 0; i < count; ++i)
    {
        if (!error_logger_read(&boot_errors, (size_t) i, message, sizeof(message)))
        {
            ++pending;
            continue;
        }

        const size_t length = strlen(message);
        snprintf_(line, sizeof(line), "[error %llu] %s%s", (unsigned long long) (i + 1), message, length != 0 && message[length - 1] == '\n' ? "" : "\n");
        error_logger_output(line);
    }

    if (reserved > count || pending != 0)
    {
        snprintf_(line, sizeof(line), "Error log: %llu dropped (buffer full), %llu pending at snapshot.\n", (unsigned long long) (reserved - count), (unsigned long long) pending);
        error_logger_output(line);
    }

    error_logger_output("======== End kernel error log ========\n");
}

static void sift_u32(uint32_t* values, size_t root, size_t count)
{
    while (root < count / 2)
    {
        size_t child = root * 2 + 1;

        if (child + 1 < count && values[child] < values[child + 1])
        {
            ++child;
        }

        if (values[root] >= values[child])
        {
            break;
        }

        uint32_t value = values[root];
        values[root]   = values[child];
        values[child]  = value;
        root           = child;
    }
}

void sort_u32(uint32_t* values, size_t count)
{
    if (values == NULL || count < 2)
    {
        return;
    }

    for (size_t start = count / 2; start != 0; --start)
    {
        sift_u32(values, start - 1, count);
    }

    for (size_t end = count; end > 1; --end)
    {
        uint32_t value  = values[0];
        values[0]       = values[end - 1];
        values[end - 1] = value;
        sift_u32(values, 0, end - 1);
    }
}

// An empty range still requires its address to be inside the inclusive bounds.
// Subtraction avoids overflow, including upper == UINT64_MAX.
bool range_within_u64(uint64_t address, uint64_t length, uint64_t lower, uint64_t upper)
{
    return address >= lower && address <= upper && (length == 0 || length - 1 <= upper - address);
}

static bool validate_user_memory(const user_memory_access_t* access, uint64_t address, size_t length, bool write)
{
    if (access == NULL || access->space == NULL || !range_within_u64(address, length, CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return false;
    }

    for (uint64_t page = align_down(address, PAGE_SIZE); length != 0 && page < address + length; page += PAGE_SIZE)
    {
        spinlock_acquire_preempt(&access->space->lock);
        bool accessible = arch_user_page_accessible(page, access->space->pt, write);
        spinlock_release_preempt(&access->space->lock);

        if (!accessible)
        {
            return false;
        }
    }

    return true;
}

static bool copy_user_locked(const user_memory_access_t* access, uint64_t address, void* buffer, size_t length, bool write)
{
    if (buffer == NULL || !validate_user_memory(access, address, length, write))
    {
        return false;
    }

    uint8_t* bytes = (uint8_t*) buffer;

    while (length != 0)
    {
        size_t      offset   = address & (PAGE_SIZE - 1);
        size_t      chunk    = length < PAGE_SIZE - offset ? length : PAGE_SIZE - offset;
        phys_addr_t physical = vmm_virt_to_phys(align_down(address, PAGE_SIZE), access->space);

        if (physical == 0)
        {
            return false;
        }

        uint8_t* page = (uint8_t*) pa_to_hhdm(physical, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);

        if (write)
        {
            memcpy(page + offset, bytes, chunk);
        }
        else
            memcpy(bytes, page + offset, chunk);
        bytes += chunk;
        address += chunk;
        length -= chunk;
    }

    return true;
}

bool safe_read_user_locked(const user_memory_access_t* access, uint64_t address, void* destination, size_t length)
{
    return copy_user_locked(access, address, destination, length, false);
}

bool safe_atomic_load_user32_locked(const user_memory_access_t* access, uint64_t address, uint32_t* value)
{
    if (value == NULL || (address & 3) != 0 || !validate_user_memory(access, address, sizeof(*value), false))
    {
        return false;
    }

    const phys_addr_t physical = vmm_virt_to_phys(address, access->space);

    if (physical == 0)
    {
        return false;
    }

    const uint32_t* word = (const uint32_t*) pa_to_hhdm(physical, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
    *value               = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    return true;
}

bool safe_atomic_compare_exchange_user32_locked(const user_memory_access_t* access, uint64_t address, uint32_t* expected, uint32_t desired)
{
    if (expected == NULL || (address & 3) != 0 || !validate_user_memory(access, address, sizeof(*expected), true))
    {
        return false;
    }

    const phys_addr_t physical = vmm_virt_to_phys(address, access->space);

    if (physical == 0)
    {
        return false;
    }

    uint32_t* word = (uint32_t*) pa_to_hhdm(physical, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
    (void) __atomic_compare_exchange_n(word, expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return true;
}

bool safe_write_user_locked(const user_memory_access_t* access, uint64_t address, const void* source, size_t length)
{
    return copy_user_locked(access, address, (void*) source, length, true);
}

bool safe_read_user(uint64_t address, void* destination, size_t length)
{
    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        return false;
    }

    bool result = safe_read_user_locked(&access, address, destination, length);
    end_user_memory(&access);
    return result;
}

bool safe_write_user(uint64_t address, const void* source, size_t length)
{
    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        return false;
    }

    bool result = safe_write_user_locked(&access, address, source, length);
    end_user_memory(&access);
    return result;
}

bool safe_read_user_string(uint64_t address, char* destination, size_t capacity, size_t* out_length)
{
    if (destination == NULL || capacity == 0)
    {
        return false;
    }

    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        return false;
    }

    bool result = false;

    for (size_t copied = 0; copied < capacity; ++copied)
    {
        if (address > UINT64_MAX - copied || !safe_read_user_locked(&access, address + copied, &destination[copied], 1))
        {
            break;
        }

        if (destination[copied] == '\0')
        {
            if (out_length != NULL)
            {
                *out_length = copied;
            }

            result = true;
            break;
        }
    }

    if (!result)
    {
        destination[capacity - 1] = '\0';
    }

    end_user_memory(&access);
    return result;
}

bool copy_to_process_virtual(virt_addr_space_t* addressSpace, uint64_t destinationVirtualAddress, const void* source, uint64_t size)
{
    if (addressSpace == NULL || source == NULL)
    {
        return false;
    }

    const uint8_t* sourceBytes = (const uint8_t*) source;
    uint64_t       offset      = 0;

    while (offset < size)
    {
        const uint64_t currentVirtualAddress = destinationVirtualAddress + offset;
        const uint64_t pageOffset            = currentVirtualAddress & (PAGE_SIZE - 1);
        const uint64_t chunkSize             = ((size - offset) < (PAGE_SIZE - pageOffset)) ? (size - offset) : (PAGE_SIZE - pageOffset);

        phys_addr_t physicalAddress = vmm_virt_to_phys(currentVirtualAddress, addressSpace);

        if (physicalAddress == 0)
        {
            return false;
        }

        uint8_t* destination = (uint8_t*) pa_to_hhdm((uintptr_t) physicalAddress, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);

        if (destination == NULL)
        {
            return false;
        }

        memcpy(destination, sourceBytes + offset, (size_t) chunkSize);
        offset += chunkSize;
    }

    return true;
}

// klib printf only prints to qemu serial.
// using printf library which uses our arch_serial_putchar()
// In future will likely use this function to log to debug buffer for dmesg
// https://en.cppreference.com/w/c/variadic/va_list.html
int kprintf(const char* format, ...)
{
    const bool emergency = platform.cpus != NULL && platform.cpus[arch_cpu_id()].emergency_logging;

    if (!emergency)
    {
        spinlock_acquire_preempt(&kprintf_lock);
    }

    va_list args;
    va_start(args, format);
    const int written = vprintf_(format, args);
    va_end(args);

    if (!emergency)
    {
        spinlock_release_preempt(&kprintf_lock);
    }

    return written;
}

void kterm_write(const char* msg)
{
    if (msg == NULL)
    {
        return;
    }

    size_t len = strlen(msg);

    if (len == 0)
    {
        return;
    }

    spinlock_acquire_preempt(&platform.terminal_lock);

    if (platform.terminal_context != NULL)
    {
        char previous = '\0';

        for (size_t i = 0; i < len; i++)
        {
            char c = msg[i];

            if (c == '\n' && previous != '\r')
            {
                static const char crlf[] = "\r\n";
                terminal_write(crlf, 2);
            }
            else
            {
                terminal_write(&c, 1);
            }

            previous = c;
        }
    }

    spinlock_release_preempt(&platform.terminal_lock);
}

int kterm_printf(const char* format, ...)
{
    if (format == NULL)
    {
        return 0;
    }

    va_list args;
    va_start(args, format);
    const int written = kterm_vprintf(format, args);
    va_end(args);

    return written;
}

int kterm_vprintf(const char* format, va_list args)
{
    if (format == NULL)
    {
        return 0;
    }

    char      out[512];
    const int result = vsnprintf_(out, sizeof(out), format, args);

    if (result <= 0)
    {
        return result;
    }

    kterm_write(out);
    return result;
}

// memory
void* memset(void* dest, int value, size_t count)
{
    uint8_t* ptr = (uint8_t*) dest;

    for (size_t i = 0; i < count; i++)
    {
        ptr[i] = (uint8_t) value;
    }

    return dest;
}

void* memcpy(void* dest, const void* src, size_t count)
{
    uint8_t*       d = (uint8_t*) dest;
    const uint8_t* s = (const uint8_t*) src;

    for (size_t i = 0; i < count; i++)
    {
        d[i] = s[i];
    }

    return dest;
}

int memcmp(const void* lhs, const void* rhs, size_t count)
{
    const uint8_t* a = (const uint8_t*) lhs;
    const uint8_t* b = (const uint8_t*) rhs;

    for (size_t i = 0; i < count; i++)
    {
        if (a[i] != b[i])
        {
            return (int) a[i] - (int) b[i];
        }
    }

    return 0;
}

size_t strlen(const char* str)
{
    if (str == NULL)
    {
        return 0;
    }

    size_t len = 0;

    while (str[len] != '\0')
    {
        len++;
    }

    return len;
}

int strcmp(const char* lhs, const char* rhs)
{
    if (lhs == NULL && rhs == NULL)
    {
        return 0;
    }

    if (lhs == NULL)
    {
        return -1;
    }

    if (rhs == NULL)
    {
        return 1;
    }

    while (*lhs != '\0' && *lhs == *rhs)
    {
        lhs++;
        rhs++;
    }

    return (int) (unsigned char) *lhs - (int) (unsigned char) *rhs;
}

char* kstrdup(const char* str)
{
    if (str == NULL)
    {
        return NULL;
    }

    size_t len = strlen(str);
    char*  out = (char*) kmalloc(len + 1);

    if (out == NULL)
    {
        return NULL;
    }

    memcpy(out, str, len + 1);
    return out;
}

// Physical address to higher half direct map
uintptr_t pa_to_hhdm(uintptr_t pa, bool hhdm_present, uint64_t hhdm_offset)
{
    if (hhdm_present)
    {
        return pa + hhdm_offset;
    }
    else
    {
        return pa;
    }
}

uintptr_t hhdm_to_pa(uintptr_t hhdm_addr, bool hhdm_present, uint64_t hhdm_offset)
{
    if (hhdm_present)
    {
        return hhdm_addr - hhdm_offset;
    }
    else
    {
        return hhdm_addr;
    }
}

static void release_virtual_region(virt_addr_space_t* space, virt_addr_t base, size_t mapped_size, virt_backing_block_t* blocks)
{
    if (mapped_size != 0)
    {
        vmm_unmap_range(base, mapped_size, space);
    }

    while (blocks != NULL)
    {
        virt_backing_block_t* next = blocks->next;
        pmm_free(blocks->pointer);
        kfree(blocks);
        blocks = next;
    }

    vmm_free_region(space, base);
}

void* vmalloc(size_t size)
{
    if (size == 0 || size > SIZE_MAX - (PAGE_SIZE - 1))
    {
        return NULL;
    }

    virt_addr_space_t* space        = vmm_kernel_space();
    const zone_t*      zone         = &platform.cpus[arch_cpu_id()].numa_node->zone;
    const size_t       aligned_size = align_up(size, PAGE_SIZE);
    spinlock_acquire_preempt(&space->operation_lock);
    virt_addr_t base = vmm_reserve_region(space, aligned_size, VIRT_ADDR_KERNEL);

    if (base == 0)
    {
        spinlock_release_preempt(&space->operation_lock);
        return NULL;
    }

    virt_backing_block_t* blocks      = NULL;
    size_t                mapped_size = 0;

    while (mapped_size < aligned_size)
    {
        const size_t          max_chunk = ((size_t) 1 << MAX_ORDER) * PAGE_SIZE;
        const size_t          remaining = aligned_size - mapped_size;
        const size_t          chunk     = remaining < max_chunk ? remaining : max_chunk;
        virt_backing_block_t* block     = kmalloc(sizeof(*block));

        if (block == NULL)
        {
            goto fail;
        }

        block->pointer = pmm_alloc(chunk);

        if (block->pointer == NULL)
        {
            kfree(block);
            goto fail;
        }

        block->next              = blocks;
        blocks                   = block;
        const phys_addr_t pa     = hhdm_to_pa((uintptr_t) block->pointer, zone->hhdm_present, zone->hhdm_offset);
        const bool        mapped = vmm_map_range(base + mapped_size, pa, chunk, vmm_kernel_page_flags(false, true), space);
        mapped_size += chunk; // Include any partially installed range in rollback.

        if (!mapped)
        {
            goto fail;
        }
    }

    vmm_find_region(space, base)->blocks = blocks;
    spinlock_release_preempt(&space->operation_lock);
    return (void*) (uintptr_t) base;
fail:
    release_virtual_region(space, base, mapped_size, blocks);
    spinlock_release_preempt(&space->operation_lock);
    return NULL;
}

void vfree(void* pointer)
{
    if (pointer == NULL)
    {
        return;
    }

    virt_addr_space_t* space = vmm_kernel_space();
    spinlock_acquire_preempt(&space->operation_lock);
    virt_region_t* region = vmm_find_region(space, (uintptr_t) pointer);

    if (region != NULL && region->start == (uintptr_t) pointer && region->blocks != NULL)
    {
        release_virtual_region(space, region->start, region->size, region->blocks);
    }

    spinlock_release_preempt(&space->operation_lock);
}

void* kmalloc(size_t size)
{
    cpu_info_t* cpu = &platform.cpus[arch_cpu_id()];

    if (cpu->numa_node == NULL)
    {
        return NULL;
    }

    if (size == 0)
    {
        return NULL;
    }

    kernel_heap_t* heap = &cpu->numa_node->heap;

    spinlock_acquire_preempt(&heap->lock);
    void* ptr = heap_alloc(heap, size);
    spinlock_release_preempt(&heap->lock);

    return ptr;
}

void* kzalloc(size_t size)
{
    void* ptr = kmalloc(size);

    if (ptr == NULL)
    {
        return NULL;
    }

    memset(ptr, 0, size);
    return ptr;
}

void kfree(void* ptr)
{
    cpu_info_t* cpu = &platform.cpus[arch_cpu_id()];

    if (cpu->numa_node == NULL)
    {
        return;
    }

    if (ptr == NULL)
    {
        return;
    }

    kernel_heap_t* heap = &cpu->numa_node->heap;

    spinlock_acquire_preempt(&heap->lock);
    heap_free(heap, ptr);
    spinlock_release_preempt(&heap->lock);
}
