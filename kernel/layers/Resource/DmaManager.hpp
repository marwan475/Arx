#pragma once

#include <dma/dma.h>
#include <debug/kernel/stats/DmaDebugStats.hpp>
#include <klib/spinlock.h>

class PhysicalMemoryManager;
class VirtualMemoryManager;
class DebugManager;

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

    ARX_DEBUG_ONLY(
        void PublishDebug(DebugManager& manager);
        const DmaDebugStats& DebugStatistics() const
        {
            return DebugStats;
        }
    )

    DmaManager(const DmaManager&)            = delete;
    DmaManager& operator=(const DmaManager&) = delete;

private:
    struct AllocationRecord
    {
        DmaAllocation          allocation;
        DmaDevice              device;
        platform_dma_mapping_t mapping;
        void*                  backing;
        phys_addr_t            physical;
        size_t                 reservedSize;
        size_t                 mappedSize;
        bool                   releasing;
        AllocationRecord*      next;
        ARX_DEBUG_ONLY(DmaAllocationDebugStats debug;)
    };
    void Track(AllocationRecord* record, DmaAllocation* allocation);

    PhysicalMemoryManager* Physical;
    VirtualMemoryManager*  Virtual;
    platform_dma_ops_t     Operations;
    mutable spinlock_t     Lock;
    AllocationRecord*      Allocations;
    size_t                 AllocationCount;
    ARX_DEBUG_ONLY(
        DmaDebugStats DebugStats;
        arx_debug_view_t DebugView;
        DmaStatus DebugOutcome(bool allocating, DmaStatus status);
    )

#if DEBUG
public:
    // Public layout alias for compiled native readers; no target calls needed.
    using DebugAllocationRecord = AllocationRecord;
#endif
};
