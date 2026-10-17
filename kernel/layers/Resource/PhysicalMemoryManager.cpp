#include "layers/Resource/PhysicalMemoryManager.hpp"

extern "C"
{
#include <memory/vmm.h>
#include <platform.h>
}

void PhysicalMemoryManager::GetMemoryInfo(uint64_t* totalBytes, uint64_t* freeBytes)
{
    *totalBytes = 0;
    *freeBytes  = 0;

    for (size_t node = 0; node < platform.numa_node_count; ++node)
    {
        zone_t* zones[] = { &platform.numa_nodes[node].zone, &platform.numa_nodes[node].dma_zone };

        for (size_t i = 0; i < 2; i++)
        {
            zone_t* zone = zones[i];
            spinlock_acquire_preempt(&zone->lock);
            *totalBytes += (uint64_t) zone->total_memory;
            *freeBytes += (uint64_t) zone->free_pages * PAGE_SIZE;
            spinlock_release_preempt(&zone->lock);
        }
    }
}

uint64_t PhysicalMemoryManager::GetSharedMemoryBytes() const
{
    return vmm_shared_memory_bytes();
}

void* PhysicalMemoryManager::Alloc(size_t size)
{
    return pmm_alloc(size);
}

void PhysicalMemoryManager::Free(void* addr)
{
    pmm_free(addr);
}

void* PhysicalMemoryManager::AllocDma(size_t size, phys_addr_t maximumAddress, phys_addr_t* physicalAddress)
{
    return pmm_alloc_dma(size, maximumAddress, physicalAddress);
}
