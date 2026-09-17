#include <klib/klib.h>

void* operator new(size_t size)
{
    return kzalloc(size);
}

void* operator new[](size_t size)
{
    return kzalloc(size);
}

void operator delete(void* ptr) noexcept
{
    kfree(ptr);
}

void operator delete[](void* ptr) noexcept
{
    kfree(ptr);
}

void operator delete(void* ptr, size_t) noexcept
{
    kfree(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
    kfree(ptr);
}
