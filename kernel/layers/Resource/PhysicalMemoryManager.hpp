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
    void*    Alloc(size_t size);
    void*    AllocDma(size_t size, phys_addr_t maximumAddress, phys_addr_t* physicalAddress);
    void     Free(void* addr);
    void     GetMemoryInfo(uint64_t* totalBytes, uint64_t* freeBytes);
    uint64_t GetSharedMemoryBytes() const;
};
