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
