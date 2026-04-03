#ifndef KLIB_H
#define KLIB_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef _Atomic uint8_t spinlock_t;

// logging
int kprintf(const char* format, ...);

// spinlock 
void spinlock_aquire(spinlock_t* lock);
void spinlock_release(spinlock_t* lock);

#endif