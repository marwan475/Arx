#pragma once

#include <dma/dma.h>
#include <klib/spinlock.h>

class PhysicalMemoryManager;
class VirtualMemoryManager;

using DmaDevice  = dma_device_t;
using DmaAddress = dma_address_t;

enum class DmaStatus
{
    Success           = DMA_SUCCESS,
    InvalidArgument   = DMA_INVALID_ARGUMENT,
    Unsupported       = DMA_UNSUPPORTED,
    NoMemory          = DMA_NO_MEMORY,
    MappingFailed     = DMA_MAPPING_FAILED,
    InvalidAllocation = DMA_INVALID_ALLOCATION,
    Busy              = DMA_BUSY
};

struct DmaAllocation
{
    void*      cpuAddress;
    DmaAddress dmaAddress;
    size_t     size;
    uint64_t   id;
};

class DmaManager
{
public:
    DmaManager(PhysicalMemoryManager* physical, VirtualMemoryManager* virtualMemory, const platform_dma_ops_t* operations);
    // All devices must be quiesced and allocations released before destruction.
    ~DmaManager();

    // Task/init context only. Output must be zero-initialized. Buffers are zeroed.
    // A failed rollback may return an id-only cleanup handle: retry FreeCoherent.
    DmaStatus AllocateCoherent(const DmaDevice& device, size_t size, size_t alignment, size_t boundary, DmaAllocation* allocation);
    // Caller must stop device access first. Failure preserves the handle/backing.
    DmaStatus FreeCoherent(DmaAllocation* allocation);
    // Descriptor stores -> publication/doorbell; completion observation -> loads.
    void   WriteBarrier();
    void   ReadBarrier();
    size_t CountAllocations() const;

    DmaManager(const DmaManager&)            = delete;
    DmaManager& operator=(const DmaManager&) = delete;

private:
    struct AllocationRecord;
    void Track(AllocationRecord* record, DmaAllocation* allocation);

    PhysicalMemoryManager* Physical;
    VirtualMemoryManager*  Virtual;
    platform_dma_ops_t     Operations;
    mutable spinlock_t     Lock;
    AllocationRecord*      Allocations;
    size_t                 AllocationCount;
};
