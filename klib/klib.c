#include "klib.h"

#include "printf.h"

// klib printf only prints to qemu serial.
// using printf library which uses our arch_serial_putchar()
// In future will like use this function to log to debug buffer for dmesg
int kprintf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vprintf_(format, args);
    va_end(args);
    return written;
}
