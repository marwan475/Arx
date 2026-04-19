#include <klib/klib.h>
#include <klib/printf/printf.h>
#include <memory/pmm.h>
#include <memory/vmm.h>

spinlock_t kprintf_lock = 0;

// klib printf only prints to qemu serial.
// using printf library which uses our arch_serial_putchar()
// In future will likely use this function to log to debug buffer for dmesg
// https://en.cppreference.com/w/c/variadic/va_list.html
int kprintf(const char* format, ...)
{
    spinlock_acquire(&kprintf_lock);
    va_list args;
    va_start(args, format);
    const int written = vprintf_(format, args);
    va_end(args);
    spinlock_release(&kprintf_lock);
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

    spinlock_acquire(&dispatcher.terminal_lock);

    if (dispatcher.terminal_context != NULL)
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

    spinlock_release(&dispatcher.terminal_lock);
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

static void rollback_vmalloc(zone_t* zone, virt_addr_space_t* address_space, virt_addr_t base, size_t mapped_size)
{
    for (size_t offset = 0; offset < mapped_size;)
    {
        const virt_addr_t va   = base + offset;
        const phys_addr_t pa   = vmm_virt_to_phys(va, address_space);
        size_t            step = PAGE_SIZE;

        if (pa != 0)
        {
            const uint64_t pfn = pa >> PAGE_SHIFT;
            if (pfn < zone->max_pfn)
            {
                page_t* page = &zone->buddy_metadata[pfn];
                if (page->flags == PMM_PAGE_USED && page->order <= MAX_ORDER)
                {
                    const uint64_t block_pages = (uint64_t) 1ULL << page->order;
                    if ((pfn & (block_pages - 1)) == 0)
                    {
                        pmm_free((void*) (uintptr_t) pa_to_hhdm(pa, zone->hhdm_present, zone->hhdm_offset));
                        step = (size_t) block_pages * PAGE_SIZE;
                    }
                }
            }
        }

        if (step == 0)
        {
            step = PAGE_SIZE;
        }

        offset += step;
    }

    if (mapped_size > 0)
    {
        vmm_unmap_range(base, mapped_size, address_space);
    }

    vmm_free_region(address_space, base);
}

void* vmalloc(size_t size)
{
    cpu_info_t* cpu = &dispatcher.cpus[arch_cpu_id()];
    if (cpu->numa_node == NULL || cpu->address_space == NULL)
    {
        return NULL;
    }

    zone_t*            zone          = &cpu->numa_node->zone;
    virt_addr_space_t* address_space = cpu->address_space;

    if (zone == NULL || address_space == NULL)
    {
        return NULL;
    }

    if (size == 0)
    {
        return NULL;
    }

    size_t aligned_size = align_up(size, PAGE_SIZE);
    if (aligned_size == 0)
    {
        return NULL;
    }

    virt_addr_t base = vmm_reserve_region(address_space, aligned_size, VIRT_ADDR_KERNEL);
    if (base == 0)
    {
        return NULL;
    }

    uint64_t page_flags = 0;
    ARCH_PAGE_FLAGS_INIT(page_flags);
    ARCH_PAGE_FLAG_SET_READ(page_flags);
    ARCH_PAGE_FLAG_SET_WRITE(page_flags);
    ARCH_PAGE_FLAG_SET_GLOBAL(page_flags);

    size_t mapped_size      = 0;
    size_t max_chunk_size   = ((size_t) 1ULL << MAX_ORDER) * PAGE_SIZE;
    size_t max_order_chunks = aligned_size / max_chunk_size;
    size_t remainder_size   = aligned_size % max_chunk_size;

    for (size_t i = 0; i < max_order_chunks; i++)
    {
        void* chunk_hhdm = pmm_alloc(max_chunk_size);
        if (chunk_hhdm == NULL)
        {
            rollback_vmalloc(zone, address_space, base, mapped_size);
            return NULL;
        }

        phys_addr_t pa = hhdm_to_pa((uintptr_t) chunk_hhdm, zone->hhdm_present, zone->hhdm_offset);
        vmm_map_range(base + mapped_size, pa, max_chunk_size, page_flags, address_space);
        mapped_size += max_chunk_size;
    }

    if (remainder_size > 0)
    {
        void* chunk_hhdm = pmm_alloc(remainder_size);
        if (chunk_hhdm == NULL)
        {
            rollback_vmalloc(zone, address_space, base, mapped_size);
            return NULL;
        }

        phys_addr_t pa = hhdm_to_pa((uintptr_t) chunk_hhdm, zone->hhdm_present, zone->hhdm_offset);
        vmm_map_range(base + mapped_size, pa, remainder_size, page_flags, address_space);
        mapped_size += remainder_size;
    }

    return (void*) (uintptr_t) base;
}

void vfree(void* ptr)
{
    cpu_info_t* cpu = &dispatcher.cpus[arch_cpu_id()];
    if (cpu->numa_node == NULL || cpu->address_space == NULL)
    {
        return;
    }

    zone_t*            zone          = &cpu->numa_node->zone;
    virt_addr_space_t* address_space = cpu->address_space;

    if (zone == NULL || address_space == NULL)
    {
        return;
    }

    if (ptr == NULL)
    {
        return;
    }

    virt_region_t* region = vmm_find_region(address_space, (virt_addr_t) (uintptr_t) ptr);
    if (region == NULL)
    {
        return;
    }

    const virt_addr_t region_start = region->start;
    const size_t      region_size  = region->size;

    for (size_t offset = 0; offset < region_size;)
    {
        const virt_addr_t va   = region_start + offset;
        const phys_addr_t pa   = vmm_virt_to_phys(va, address_space);
        size_t            step = PAGE_SIZE;

        if (pa != 0)
        {
            const uint64_t pfn = pa >> PAGE_SHIFT;
            if (pfn < zone->max_pfn)
            {
                page_t* page = &zone->buddy_metadata[pfn];
                if (page->flags == PMM_PAGE_USED && page->order <= MAX_ORDER)
                {
                    const uint64_t block_pages = (uint64_t) 1ULL << page->order;
                    if ((pfn & (block_pages - 1)) == 0)
                    {
                        pmm_free((void*) (uintptr_t) pa_to_hhdm(pa, zone->hhdm_present, zone->hhdm_offset));
                        step = (size_t) block_pages * PAGE_SIZE;
                    }
                }
            }
        }

        if (step == 0)
        {
            step = PAGE_SIZE;
        }

        offset += step;
    }

    vmm_unmap_range(region_start, region_size, address_space);
    vmm_free_region(address_space, region_start);
}