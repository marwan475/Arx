#include <klib/klib.h>
#include <klib/printf.h>
#include <boot/boot.h>

spinlock_t kprintf_lock = 0;

// klib printf only prints to qemu serial.
// using printf library which uses our arch_serial_putchar()
// In future will like use this function to log to debug buffer for dmesg
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

//https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html
void spinlock_aquire(spinlock_t* lock)
{
    // loop so that once lock is free we aquire it
    for (;;)
    {
        uint8_t expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        {
            return;
        }

        while (__atomic_load_n(lock, __ATOMIC_RELAXED))
        {
            arch_pause();
        }
    }
}

void spinlock_release(spinlock_t* lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

