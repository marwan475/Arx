#include "layers/Resource/DmaManager.hpp"

#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <klib/klib.h>

#if DEBUG
#include <debug/kernel/DebugManager.hpp>
#include <platform.h>
#endif


static uint64_t NextAllocationId;

#if DEBUG
#define DMA_ALLOCATION_RESULT(value) DebugOutcome(true, (value))
#define DMA_FREE_RESULT(value) DebugOutcome(false, (value))
#else
#define DMA_ALLOCATION_RESULT(value) (value)
#define DMA_FREE_RESULT(value) (value)
#endif

static bool PowerOfTwo(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

DmaManager::DmaManager(PhysicalMemoryManager* physical, VirtualMemoryManager* virtualMemory, const platform_dma_ops_t* operations)
{
    Physical        = physical;
    Virtual         = virtualMemory;
    Operations      = operations != nullptr ? *operations : platform_dma_ops_t {};
    Lock            = 0;
    Allocations     = nullptr;
    AllocationCount = 0;
    ARX_DEBUG_RUN(
        DebugStats = {};
        DebugStats.source = this;
        DebugStats.backendKind = Operations.map == nullptr ? 0 : operations == &platform.dma_ops ? 1 : 2;
        DebugView = {};
    );
}

DmaManager::~DmaManager()
{
    // Reclaiming live buffers could DMA into freed memory. Treat this as misuse.
    if (Allocations != nullptr)
    {
        error_logger("Arx kernel: destroying DMA manager with live allocations\n");
        panic();
    }
}

void DmaManager::Track(AllocationRecord* record, DmaAllocation* allocation)
{
    spinlock_acquire_preempt(&Lock);
    record->next = Allocations;
    Allocations  = record;
    AllocationCount++;
    ARX_DEBUG_RUN(
        record->debug.cleanupOnly = record->allocation.cpuAddress == nullptr;
        record->debug.backingHeld = true;
        record->debug.mappingHeld = true;
        record->debug.phase = record->debug.cleanupOnly ? DmaDebugPhase::RollbackRetained : DmaDebugPhase::Live;

        if (record->debug.cleanupOnly)
        {
            ARX_DEBUG_ADD(DebugStats.rollbackRetained, 1);
            ARX_DEBUG_ADD(DebugStats.cleanupRecords, 1);
        }
        else
        {
            ARX_DEBUG_ADD(DebugStats.successfulRecords, 1);
        }

        ARX_DEBUG_ADD(DebugStats.retainedRequestedBytes, record->mappedSize);
        ARX_DEBUG_ADD(DebugStats.retainedMappedBytes, record->mappedSize);
        ARX_DEBUG_ADD(DebugStats.retainedReservedBytes, record->reservedSize);
    );
    *allocation = record->allocation;
    spinlock_release_preempt(&Lock);
}

DmaStatus DmaManager::AllocateCoherent(const DmaDevice& device, size_t size, size_t alignment, size_t boundary, DmaAllocation* allocation)
{
    ARX_DEBUG_ONLY(DebugDmaOperation debugOperation(DebugStats);)
    ARX_DEBUG_ADD(DebugStats.allocationCalls, 1);

    if (allocation == nullptr || allocation->cpuAddress != nullptr || allocation->dmaAddress.value != 0 || allocation->size != 0 || allocation->id != 0 || size == 0 || !PowerOfTwo(alignment) || !PowerOfTwo(device.minimumAlignment) || (boundary != 0 && !PowerOfTwo(boundary)) || (device.boundary != 0 && !PowerOfTwo(device.boundary)) || device.addressMask == 0 || (device.addressMask & (device.addressMask + 1)) != 0)
    {
        return DMA_ALLOCATION_RESULT(DmaStatus::InvalidArgument);
    }

    ARX_DEBUG_ONLY(const size_t requestedAlignment = alignment;)
    ARX_DEBUG_ONLY(const size_t requestedBoundary = boundary;)

    alignment = alignment > device.minimumAlignment ? alignment : device.minimumAlignment;

    if (device.boundary != 0 && (boundary == 0 || device.boundary < boundary))
    {
        boundary = device.boundary;
    }

    if (boundary != 0 && size > boundary)
    {
        return DMA_ALLOCATION_RESULT(DmaStatus::InvalidArgument);
    }

    const size_t maximum = PAGE_SIZE * (1UL << MAX_ORDER);

    if (size > maximum || alignment > maximum)
    {
        return DMA_ALLOCATION_RESULT(DmaStatus::Unsupported);
    }

    if (Physical == nullptr || Virtual == nullptr || Operations.query == nullptr || Operations.map == nullptr || Operations.unmap == nullptr || Operations.writeBarrier == nullptr || Operations.readBarrier == nullptr)
    {
        return DMA_ALLOCATION_RESULT(DmaStatus::Unsupported);
    }

    platform_dma_caps_t   caps {};
    platform_dma_status_t status = Operations.query(Operations.context, &device, &caps);

    if (status != DMA_SUCCESS)
    {
        return DMA_ALLOCATION_RESULT(static_cast<DmaStatus>(status));
    }

    if (!caps.coherent || vmm_kernel_space() == nullptr)
    {
        return DMA_ALLOCATION_RESULT(DmaStatus::Unsupported);
    }

    size_t reserved = PAGE_SIZE;

    while (reserved < size || reserved < alignment)
    {
        reserved *= 2;
    }

    AllocationRecord* record = static_cast<AllocationRecord*>(kzalloc(sizeof(AllocationRecord)));

    if (record == nullptr)
    {
        return DMA_ALLOCATION_RESULT(DmaStatus::NoMemory);
    }

    // IDs are global across managers, never reused, and stop before wraparound.
    uint64_t id = __atomic_load_n(&NextAllocationId, __ATOMIC_RELAXED);

    for (;;)
    {
        if (id == UINT64_MAX)
        {
            kfree(record);
            return DMA_ALLOCATION_RESULT(DmaStatus::NoMemory);
        }

        if (__atomic_compare_exchange_n(&NextAllocationId, &id, id + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        {
            break;
        }
    }

    record->backing = Physical->AllocDma(reserved, caps.physicalAddressMask, &record->physical);

    if (record->backing == nullptr)
    {
        kfree(record);
        return DMA_ALLOCATION_RESULT(DmaStatus::NoMemory);
    }

    record->reservedSize = reserved;
    ARX_DEBUG_RUN(
        record->debug.requestedAlignment = requestedAlignment;
        record->debug.requestedBoundary = requestedBoundary;
        record->debug.effectiveAlignment = alignment;
        record->debug.effectiveBoundary = boundary;
        record->debug.physicalMask = caps.physicalAddressMask;
    );
    uintptr_t cpuAddress = (uintptr_t) record->backing;
    bool      valid      = (cpuAddress & (alignment - 1)) == 0 && cpuAddress <= UINTPTR_MAX - (reserved - 1) && record->physical <= UINT64_MAX - (reserved - 1);

    for (size_t offset = 0; valid && offset < reserved; offset += PAGE_SIZE)
    {
        // Translation returns zero for both physical page zero and failure.
        // Verify a nonzero offset too, so page zero is never mistaken for a hole.
        valid = Virtual->VirtToPhys((virt_addr_t) record->backing + offset, vmm_kernel_space()) == record->physical + offset && Virtual->VirtToPhys((virt_addr_t) record->backing + offset + 1, vmm_kernel_space()) == record->physical + offset + 1;
    }

    if (!valid)
    {
        Physical->Free(record->backing);
        kfree(record);
        return DMA_ALLOCATION_RESULT(DmaStatus::MappingFailed);
    }

    memset(record->backing, 0, reserved);
    record->device        = device;
    record->mappedSize    = size;
    record->allocation.id = id + 1;
    status                = Operations.map(Operations.context, &record->device, record->physical, size, alignment, boundary, &record->mapping);

    if (status != DMA_SUCCESS)
    {
        Physical->Free(record->backing);
        kfree(record);
        return DMA_ALLOCATION_RESULT(static_cast<DmaStatus>(status));
    }

    if (!dma_range_valid(record->mapping.address, size, device.addressMask, alignment, boundary))
    {
        status = Operations.unmap(Operations.context, &record->device, &record->mapping, size);
        ARX_DEBUG_STORE(record->debug.lastCleanupStatus, status);

        if (status != DMA_SUCCESS)
        {
            // Never expose the invalid DMA range, or reuse still-mapped pages.
            Track(record, allocation);
            return DMA_ALLOCATION_RESULT(DmaStatus::MappingFailed);
        }

        Physical->Free(record->backing);
        kfree(record);
        return DMA_ALLOCATION_RESULT(DmaStatus::MappingFailed);
    }

    record->allocation.cpuAddress = record->backing;
    record->allocation.dmaAddress = record->mapping.address;
    record->allocation.size       = size;
    Track(record, allocation);
    return DMA_ALLOCATION_RESULT(DmaStatus::Success);
}

DmaStatus DmaManager::FreeCoherent(DmaAllocation* allocation)
{
    ARX_DEBUG_ONLY(DebugDmaOperation debugOperation(DebugStats);)
    ARX_DEBUG_ADD(DebugStats.freeCalls, 1);

    if (allocation == nullptr || allocation->id == 0)
    {
        return DMA_FREE_RESULT(DmaStatus::InvalidAllocation);
    }

    spinlock_acquire_preempt(&Lock);
    AllocationRecord* record = Allocations;

    while (record != nullptr && record->allocation.id != allocation->id)
    {
        record = record->next;
    }

    if (record == nullptr || record->allocation.cpuAddress != allocation->cpuAddress || record->allocation.dmaAddress.value != allocation->dmaAddress.value || record->allocation.size != allocation->size)
    {
        spinlock_release_preempt(&Lock);
        return DMA_FREE_RESULT(DmaStatus::InvalidAllocation);
    }

    if (record->releasing)
    {
        spinlock_release_preempt(&Lock);
        return DMA_FREE_RESULT(DmaStatus::Busy);
    }

    record->releasing = true;
    ARX_DEBUG_RUN(record->debug.phase = DmaDebugPhase::Unmapping);
    spinlock_release_preempt(&Lock);
    platform_dma_status_t status = Operations.unmap(Operations.context, &record->device, &record->mapping, record->mappedSize);

    ARX_DEBUG_STORE(record->debug.lastCleanupStatus, status);

    if (status != DMA_SUCCESS)
    {
        spinlock_acquire_preempt(&Lock);
        record->releasing = false;
        ARX_DEBUG_RUN(
            record->debug.phase = record->debug.cleanupOnly ? DmaDebugPhase::RollbackRetained : DmaDebugPhase::Live;
        );
        spinlock_release_preempt(&Lock);
        return DMA_FREE_RESULT(DmaStatus::MappingFailed);
    }

    ARX_DEBUG_RUN(
        record->debug.phase = DmaDebugPhase::Unmapped;
        record->debug.mappingHeld = false;
        ARX_DEBUG_ADD(DebugStats.retainedMappedBytes, (uint64_t) -record->mappedSize);
    );

    Physical->Free(record->backing);
    ARX_DEBUG_RUN(
        record->debug.phase = DmaDebugPhase::BackingReleased;
        record->debug.backingHeld = false;
        ARX_DEBUG_ADD(DebugStats.retainedReservedBytes, (uint64_t) -record->reservedSize);
    );
    spinlock_acquire_preempt(&Lock);
    AllocationRecord** link = &Allocations;

    while (*link != record)
    {
        link = &(*link)->next;
    }

    *link = record->next;
    AllocationCount--;
    ARX_DEBUG_RUN(
        ARX_DEBUG_ADD(DebugStats.retainedRequestedBytes, (uint64_t) -record->mappedSize);

        if (record->debug.cleanupOnly)
        {
            ARX_DEBUG_ADD(DebugStats.cleanupRecords, UINT64_MAX);
            ARX_DEBUG_ADD(DebugStats.cleanupCompletions, 1);
        }
        else
        {
            ARX_DEBUG_ADD(DebugStats.successfulRecords, UINT64_MAX);
        }
    );
    *allocation = DmaAllocation {};
    spinlock_release_preempt(&Lock);
    kfree(record);
    return DMA_FREE_RESULT(DmaStatus::Success);
}

void DmaManager::WriteBarrier()
{
    if (Operations.writeBarrier != nullptr)
    {
        Operations.writeBarrier(Operations.context);
    }
}

void DmaManager::ReadBarrier()
{
    if (Operations.readBarrier != nullptr)
    {
        Operations.readBarrier(Operations.context);
    }
}

size_t DmaManager::CountAllocations() const
{
    spinlock_acquire_preempt(&Lock);
    size_t count = AllocationCount;
    spinlock_release_preempt(&Lock);
    return count;
}

#if DEBUG
DmaStatus DmaManager::DebugOutcome(bool allocating, DmaStatus status)
{
    const unsigned index = static_cast<unsigned>(status);

    if (index < 7)
    {
        uint64_t* outcomes = allocating ? DebugStats.allocationOutcomes : DebugStats.freeOutcomes;
        ARX_DEBUG_ADD(outcomes[index], 1);
    }

    return status;
}

void DmaManager::PublishDebug(DebugManager& manager)
{
    DebugView = {"allocations", "DmaDebugStats", "arx.memory.dma", 1,
                 &DebugStats, sizeof(DebugStats), 0, 0};
    manager.Register("dma", "DmaDebugStats", &DebugStats, sizeof(DebugStats), 1,
                    &DebugStats.group, &DebugView, 1);
}
#endif
