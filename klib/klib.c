#include <klib/klib.h>
#include <klib/printf.h>

spinlock_t kprintf_lock = 0;

// klib printf only prints to qemu serial.
// using printf library which uses our arch_serial_putchar()
// In future will likely use this function to log to debug buffer for dmesg
// https://en.cppreference.com/w/c/variadic/va_list.html
int kprintf(const char* format, ...)
{
    spinlock_aquire(&kprintf_lock);
    va_list args;
    va_start(args, format);
    const int written = vprintf_(format, args);
    va_end(args);
    spinlock_release(&kprintf_lock);
    return written;
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

// Physical address to higher half direct map
uintptr_t pa_to_hhdm(uintptr_t pa, struct boot_info* boot_info)
{
    if (boot_info->hhdm_present)
    {
        return pa + boot_info->hhdm_offset;
    }
    else
    {
        return pa;
    }
}