#pragma once

#ifdef __cplusplus
extern "C"
{
#include <memory/pmm.h>
}
#endif

class PhysicalMemoryManager
{
public:
    void* Alloc(size_t size);
    void  Free(void* addr);
};
