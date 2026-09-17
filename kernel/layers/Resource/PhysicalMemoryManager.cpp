#include "layers/Resource/PhysicalMemoryManager.hpp"

void* PhysicalMemoryManager::Alloc(size_t size)
{
    return pmm_alloc(size);
}

void PhysicalMemoryManager::Free(void* addr)
{
    pmm_free(addr);
}
