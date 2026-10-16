#include "layers/Resource/DmaManager.hpp"
#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <platform.h>
#include <selftests/ManagerSuite.hpp>

struct DmaMock
{
    uint64_t       offset          = UINT64_C(0x100000000);
    uint64_t       physicalMask    = UINT32_MAX;
    bool           coherent        = true;
    bool           failMap         = false;
    bool           failUnmap       = false;
    bool           malformed       = false;
    size_t         maps            = 0;
    size_t         unmaps          = 0;
    size_t         live            = 0;
    size_t         writes          = 0;
    size_t         reads           = 0;
    DmaManager*    manager         = nullptr;
    DmaAllocation* releasing       = nullptr;
    DmaStatus      recursiveStatus = DmaStatus::Success;
};

static platform_dma_status_t MockQuery(void* context, const DmaDevice* device, platform_dma_caps_t* caps)
{
    (void) device;
    DmaMock* mock             = static_cast<DmaMock*>(context);
    caps->coherent            = mock->coherent;
    caps->physicalAddressMask = mock->physicalMask;
    return DMA_SUCCESS;
}

static platform_dma_status_t MockMap(void* context, const DmaDevice* device, phys_addr_t physical, size_t size, size_t alignment, size_t boundary, platform_dma_mapping_t* mapping)
{
    DmaMock* mock = static_cast<DmaMock*>(context);
    mock->maps++;

    if (mock->failMap)
    {
        return DMA_MAPPING_FAILED;
    }

    if (!mock->malformed && (physical > UINT64_MAX - mock->offset || !dma_range_valid(DmaAddress { physical + mock->offset }, size, device->addressMask, alignment, boundary)))
    {
        return DMA_MAPPING_FAILED;
    }

    mapping->address.value = mock->malformed ? UINT64_MAX : physical + mock->offset;
    mapping->cookie        = mock;
    mock->live++;
    return DMA_SUCCESS;
}

static platform_dma_status_t MockUnmap(void* context, const DmaDevice* device, const platform_dma_mapping_t* mapping, size_t size)
{
    (void) device;
    (void) size;
    DmaMock* mock = static_cast<DmaMock*>(context);
    mock->unmaps++;

    if (mock->releasing != nullptr)
    {
        DmaAllocation copy    = *mock->releasing;
        mock->recursiveStatus = mock->manager->FreeCoherent(&copy);
    }

    if (mock->failUnmap || mapping->cookie != mock)
    {
        return DMA_MAPPING_FAILED;
    }

    mock->live--;
    return DMA_SUCCESS;
}

static void MockWrite(void* context)
{
    static_cast<DmaMock*>(context)->writes++;
}

static void MockRead(void* context)
{
    static_cast<DmaMock*>(context)->reads++;
}

static platform_dma_ops_t MockOperations(DmaMock* mock)
{
    return platform_dma_ops_t { mock, MockQuery, MockMap, MockUnmap, MockWrite, MockRead };
}

static uint64_t DmaFreePages()
{
    uint64_t pages = 0;

    for (size_t i = 0; i < platform.numa_node_count; i++)
    {
        zone_t* zone = &platform.numa_nodes[i].dma_zone;
        spinlock_acquire_preempt(&zone->lock);
        pages += zone->free_pages;
        spinlock_release_preempt(&zone->lock);
    }

    return pages;
}

static bool InZone(const zone_t* zone, phys_addr_t physical)
{
    uint64_t pfn = physical / PAGE_SIZE;

    for (size_t i = 0; i < zone->region_count; i++)
    {
        if (pfn >= zone->regions[i].start_pfn && pfn < zone->regions[i].end_pfn)
        {
            return true;
        }
    }

    return false;
}

void test_DmaManager(ResourceLayerCaps* resources)
{
    auto*        manager = resources != nullptr ? resources->dmaManager : nullptr;
    ManagerSuite suite("resource/DmaManager", manager != nullptr);

    if (resources == nullptr || manager == nullptr)
    {
        return;
    }
    DmaMock            mock;
    platform_dma_ops_t operations = MockOperations(&mock);
    DmaManager         testManager(resources->physicalMemoryManager, resources->virtualMemoryManager, &operations);
    DmaDevice          device { nullptr, UINT64_MAX, 1, 0 };

    suite.Run("resource/DmaManager/platform_contract", [&](TestCase& t)
    {
        DmaDevice             actual { platform.pci_device_count != 0 ? &platform.pci_devices[0] : nullptr, UINT32_MAX, 1, 0 };
        platform_dma_caps_t   caps {};
        platform_dma_status_t status = platform.dma_ops.query(platform.dma_ops.context, &actual, &caps);
        DmaAllocation         buffer {};
        uint64_t              before = DmaFreePages();
        DmaStatus             result = manager->AllocateCoherent(actual, 3000, 64, PAGE_SIZE, &buffer);

        if (status == DMA_UNSUPPORTED)
        {
            t.Check(result == DmaStatus::Unsupported, "unsupported hardware fails explicitly");
            t.Equal(buffer.id, 0, "unsupported hardware produces no allocation");
        }
        else if (before == 0)
        {
            t.Check(result == DmaStatus::NoMemory, "absent reserved memory fails explicitly");
        }
        else if (t.Check(result == DmaStatus::Success, "production coherent allocation"))
        {
            phys_addr_t physical = resources->virtualMemoryManager->VirtToPhys((virt_addr_t) buffer.cpuAddress, vmm_kernel_space());
            t.Equal(buffer.dmaAddress.value, physical, "direct backend uses identity device address");
            manager->WriteBarrier();
            manager->ReadBarrier();
            t.Check(manager->FreeCoherent(&buffer) == DmaStatus::Success, "production release");
        }

        t.Equal(DmaFreePages(), before, "production path restores DMA accounting");
        t.Equal(manager->CountAllocations(), 0, "production tracking empty");
    });

    if (DmaFreePages() == 0)
    {
        TestCase unavailable("resource/DmaManager/reserved_memory");
        unavailable.Block("DMA zone unavailable; coherent allocation tests require reserved pages");
        return;
    }

    suite.Run("resource/DmaManager/coherent_mapping_lifetime", [&](TestCase& t)
    {
        uint64_t      before = DmaFreePages();
        DmaAllocation buffers[3] {};
        const size_t  sizes[]       = { 1, PAGE_SIZE + 17, PAGE_SIZE * 3 };
        size_t        expectedPages = 0;

        for (size_t i = 0; i < 3; i++)
        {
            if (!t.Check(testManager.AllocateCoherent(device, sizes[i], PAGE_SIZE, 65536, &buffers[i]) == DmaStatus::Success, "simultaneous coherent allocation"))
            {
                continue;
            }

            size_t reserved = PAGE_SIZE;

            while (reserved < sizes[i])
            {
                reserved *= 2;
            }

            expectedPages += reserved / PAGE_SIZE;
            t.Equal(before - DmaFreePages(), expectedPages, "actual buddy reservation accounted separately");
            t.Equal(buffers[i].size, sizes[i], "requested size preserved");
            auto* bytes  = static_cast<uint8_t*>(buffers[i].cpuAddress);
            bool  zeroed = true;

            for (size_t byte = 0; byte < reserved; byte++)
            {
                zeroed = zeroed && bytes[byte] == 0;
            }

            t.Check(zeroed, "entire reservation is zeroed");
            phys_addr_t physical = resources->virtualMemoryManager->VirtToPhys((virt_addr_t) bytes, vmm_kernel_space());
            t.Equal(buffers[i].dmaAddress.value, physical + mock.offset, "device address differs from system physical address");
            t.Check(InZone(&platform.cpus[arch_cpu_id()].numa_node->dma_zone, physical), "backing belongs to DMA zone");
            t.Check(!InZone(&platform.cpus[arch_cpu_id()].numa_node->zone, physical), "DMA backing excluded from Normal zone");

            for (size_t offset = 0; offset < reserved; offset += PAGE_SIZE)
            {
                t.Equal(resources->virtualMemoryManager->VirtToPhys((virt_addr_t) bytes + offset, vmm_kernel_space()), physical + offset, "CPU mapping is physically contiguous");
            }

            bytes[0]            = 0x5a;
            bytes[sizes[i] - 1] = 0xa5;
            t.Equal(bytes[sizes[i] - 1], 0xa5, "CPU can access final requested byte");
        }

        testManager.WriteBarrier();
        testManager.ReadBarrier();
        t.Equal(mock.writes, 1, "write ordering delegated to Platform");
        t.Equal(mock.reads, 1, "read ordering delegated to Platform");

        for (size_t i = 0; i < 3; i++)
        {
            if (buffers[i].id != 0)
            {
                t.Check(testManager.FreeCoherent(&buffers[i]) == DmaStatus::Success, "release simultaneous allocation");
                t.Check(buffers[i].cpuAddress == nullptr && buffers[i].id == 0, "release clears public record");
            }
        }

        t.Equal(DmaFreePages(), before, "all buddy pages returned");
        t.Equal(testManager.CountAllocations(), 0, "tracking empty after release");
        t.Equal(mock.live, 0, "all device mappings removed");
    });

    suite.Run("resource/DmaManager/constraints", [&](TestCase& t)
    {
        uint64_t before = DmaFreePages();
        mock.offset     = 0;
        DmaDevice     constrained { nullptr, UINT32_MAX, 65536, 65536 };
        DmaAllocation buffer {};

        if (t.Check(testManager.AllocateCoherent(constrained, 5000, 64, 131072, &buffer) == DmaStatus::Success, "combined device and allocation constraints"))
        {
            t.Equal((uintptr_t) buffer.cpuAddress % 65536, 0, "CPU alignment");
            t.Equal(buffer.dmaAddress.value % 65536, 0, "DMA alignment");
            t.Check(dma_range_valid(buffer.dmaAddress, buffer.size, UINT32_MAX, 65536, 65536), "full DMA range respects mask and boundary");
            t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::Success, "aligned allocation released");
        }

        DmaDevice tinyMask { nullptr, PAGE_SIZE - 1, 1, 0 };
        mock.physicalMask = tinyMask.addressMask;
        t.Check(testManager.AllocateCoherent(tinyMask, PAGE_SIZE * 2, 1, 0, &buffer) == DmaStatus::NoMemory, "unavailable address range fails without fallback");
        DmaStatus zeroPage = testManager.AllocateCoherent(tinyMask, PAGE_SIZE, 1, 0, &buffer);

        if (zeroPage == DmaStatus::Success)
        {
            t.Equal(buffer.dmaAddress.value, 0, "physical page zero can have a valid direct DMA address");
            t.Equal(resources->virtualMemoryManager->VirtToPhys((virt_addr_t) buffer.cpuAddress + 1, vmm_kernel_space()), 1, "page-zero CPU mapping is present");
            t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::Success, "page-zero allocation released");
        }
        else
        {
            t.Check(zeroPage == DmaStatus::NoMemory, "absent physical page zero reported without a fake mapping");
        }

        mock.physicalMask = UINT32_MAX;
        mock.offset       = UINT64_C(0x100000000);
        t.Check(testManager.AllocateCoherent(constrained, PAGE_SIZE, 64, 0, &buffer) == DmaStatus::MappingFailed, "translated address above device mask fails");
        t.Equal(DmaFreePages(), before, "constraint failures return pages");
        t.Check(dma_range_valid(DmaAddress { UINT32_MAX - 4095 }, 4096, UINT32_MAX, 4096, 4096), "last byte at mask accepted");
        t.Check(!dma_range_valid(DmaAddress { UINT32_MAX - 4095 }, 4097, UINT32_MAX, 1, 0), "range exceeding mask rejected");
        t.Check(!dma_range_valid(DmaAddress { UINT64_MAX - 7 }, 16, UINT64_MAX, 1, 0), "DMA range overflow rejected");
        t.Check(!dma_range_valid(DmaAddress { 4095 }, 2, UINT64_MAX, 1, 4096), "crossing boundary rejected");
        t.Check(dma_range_valid(DmaAddress { 0 }, 1, UINT64_MAX, 1, 0), "zero device address is valid");
    });

    suite.Run("resource/DmaManager/invalid_arguments_handles", [&](TestCase& t)
    {
        uint64_t      before = DmaFreePages();
        DmaAllocation buffer {};
        t.Check(testManager.AllocateCoherent(device, 0, 1, 0, &buffer) == DmaStatus::InvalidArgument, "zero size rejected");
        t.Check(testManager.AllocateCoherent(device, 1, 0, 0, &buffer) == DmaStatus::InvalidArgument, "zero alignment rejected");
        t.Check(testManager.AllocateCoherent(device, 1, 3, 0, &buffer) == DmaStatus::InvalidArgument, "non-power-of-two alignment rejected");
        t.Check(testManager.AllocateCoherent(device, 1, 1, 3, &buffer) == DmaStatus::InvalidArgument, "invalid boundary rejected");
        t.Check(testManager.AllocateCoherent(device, 8192, 1, 4096, &buffer) == DmaStatus::InvalidArgument, "size exceeding boundary rejected");
        t.Check(testManager.AllocateCoherent(device, SIZE_MAX, 1, 0, &buffer) == DmaStatus::Unsupported, "oversized allocation fails without arithmetic overflow");
        t.Check(testManager.AllocateCoherent(device, 1, 1, 0, nullptr) == DmaStatus::InvalidArgument, "null output rejected");
        DmaDevice invalid   = device;
        invalid.addressMask = 0x1000;
        t.Check(testManager.AllocateCoherent(invalid, 1, 1, 0, &buffer) == DmaStatus::InvalidArgument, "noncontiguous mask rejected");
        invalid                  = device;
        invalid.minimumAlignment = 3;
        t.Check(testManager.AllocateCoherent(invalid, 1, 1, 0, &buffer) == DmaStatus::InvalidArgument, "invalid device alignment rejected");
        invalid          = device;
        invalid.boundary = 3;
        t.Check(testManager.AllocateCoherent(invalid, 1, 1, 0, &buffer) == DmaStatus::InvalidArgument, "invalid device boundary rejected");
        t.Check(testManager.FreeCoherent(nullptr) == DmaStatus::InvalidAllocation, "null release rejected");
        t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::InvalidAllocation, "empty release rejected");

        DmaStatus handleStatus = testManager.AllocateCoherent(device, PAGE_SIZE, 1, 0, &buffer);
        t.Equal(static_cast<uint64_t>(handleStatus), static_cast<uint64_t>(DmaStatus::Success), "handle allocation status");
        if (t.Check(handleStatus == DmaStatus::Success, "handle test allocation"))
        {
            DmaAllocation stale   = buffer;
            DmaAllocation corrupt = buffer;
            corrupt.cpuAddress    = reinterpret_cast<void*>(1);
            t.Check(testManager.FreeCoherent(&corrupt) == DmaStatus::InvalidAllocation, "tampered pointer cannot reach PMM");
            t.Check(manager->FreeCoherent(&stale) == DmaStatus::InvalidAllocation, "foreign manager rejects handle");
            t.Check(testManager.AllocateCoherent(device, 1, 1, 0, &buffer) == DmaStatus::InvalidArgument, "live output cannot be overwritten");
            t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::Success, "valid handle released");
            t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::InvalidAllocation, "double release rejected");

            if (t.Check(testManager.AllocateCoherent(device, PAGE_SIZE, 1, 0, &buffer) == DmaStatus::Success, "replacement allocation"))
            {
                t.Check(buffer.id != stale.id, "allocation identity never reused");
                t.Check(testManager.FreeCoherent(&stale) == DmaStatus::InvalidAllocation, "stale copy cannot free replacement");
                t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::Success, "replacement released");
            }
        }

        t.Equal(DmaFreePages(), before, "invalid operations preserve PMM accounting");
    });

    suite.Run("resource/DmaManager/failure_rollback", [&](TestCase& t)
    {
        uint64_t      before = DmaFreePages();
        DmaAllocation buffer {};
        mock.coherent = false;
        t.Check(testManager.AllocateCoherent(device, 8192, 1, 0, &buffer) == DmaStatus::Unsupported, "noncoherent backend rejected");
        mock.coherent = true;
        mock.failMap  = true;
        t.Check(testManager.AllocateCoherent(device, 8192, 1, 0, &buffer) == DmaStatus::MappingFailed, "map failure reported");
        t.Equal(DmaFreePages(), before, "map failure rolls back physical allocation");
        mock.failMap   = false;
        mock.malformed = true;
        t.Check(testManager.AllocateCoherent(device, 8192, 1, 0, &buffer) == DmaStatus::MappingFailed, "invalid backend range rejected");
        t.Equal(mock.live, 0, "invalid mapping removed");
        t.Equal(DmaFreePages(), before, "invalid mapping rollback returns pages");
        mock.failUnmap = true;
        t.Check(testManager.AllocateCoherent(device, 8192, 1, 0, &buffer) == DmaStatus::MappingFailed, "failed rollback reported");
        t.Check(buffer.id != 0 && buffer.cpuAddress == nullptr && buffer.size == 0, "failed rollback returns cleanup-only handle");
        t.Equal(testManager.CountAllocations(), 1, "failed rollback remains tracked");
        mock.failUnmap = false;
        t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::Success, "cleanup-only handle releases retained mapping");
        mock.malformed = false;

        if (t.Check(testManager.AllocateCoherent(device, 8192, 1, 0, &buffer) == DmaStatus::Success, "release-failure allocation"))
        {
            uint64_t allocated = DmaFreePages();
            mock.failUnmap     = true;
            t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::MappingFailed, "unmap failure reported");
            t.Equal(DmaFreePages(), allocated, "failed unmap never frees physical backing");
            t.Equal(testManager.CountAllocations(), 1, "failed unmap keeps metadata");
            mock.failUnmap = false;
            mock.manager   = &testManager;
            mock.releasing = &buffer;
            t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::Success, "release can be retried");
            mock.releasing = nullptr;
            t.Check(mock.recursiveStatus == DmaStatus::Busy, "release in progress cannot run twice");
        }

        DmaManager unavailable(resources->physicalMemoryManager, resources->virtualMemoryManager, nullptr);
        t.Check(unavailable.AllocateCoherent(device, 1, 1, 0, &buffer) == DmaStatus::Unsupported, "missing Platform operations rejected");
        t.Equal(testManager.CountAllocations(), 0, "all failure metadata reclaimed");
        t.Equal(mock.live, 0, "all failure mappings reclaimed");
        t.Equal(DmaFreePages(), before, "all failure paths restore pages");
    });

    suite.Run("resource/DmaManager/zone_exhaustion_cycles", [&](TestCase& t)
    {
        for (size_t node = 0; node < platform.numa_node_count; node++)
        {
            zone_t*        normal   = &platform.numa_nodes[node].zone;
            zone_t*        dma      = &platform.numa_nodes[node].dma_zone;
            uint64_t       lowPages = dma->total_pages;
            const uint64_t limit    = UINT64_C(0x100000000) / PAGE_SIZE;

            for (size_t i = 0; i < normal->region_count; i++)
            {
                uint64_t end = normal->regions[i].end_pfn < limit ? normal->regions[i].end_pfn : limit;

                if (end > normal->regions[i].start_pfn)
                {
                    lowPages += end - normal->regions[i].start_pfn;
                }
            }

            uint64_t target = lowPages / 16;
            uint64_t cap    = (64UL * 1024 * 1024) / PAGE_SIZE;
            target          = target < cap ? target : cap;
            target          = target / (1UL << MAX_ORDER) * (1UL << MAX_ORDER);
            t.Equal(dma->total_pages, target, "DMA reserve follows size and rounding policy");
            t.Equal(dma->free_pages + dma->used_pages, dma->total_pages, "DMA zone accounting invariant");
            t.Equal(normal->free_pages + normal->used_pages, normal->total_pages, "Normal zone accounting invariant");

            for (size_t i = 0; i < dma->region_count; i++)
            {
                t.Check(dma->regions[i].end_pfn <= limit, "entire DMA zone is below 4 GiB");

                for (size_t j = 0; j < normal->region_count; j++)
                {
                    t.Check(dma->regions[i].end_pfn <= normal->regions[j].start_pfn || dma->regions[i].start_pfn >= normal->regions[j].end_pfn, "DMA and Normal regions never overlap");
                }
            }
        }

        // Even maximally fragmented, the node reserve contains at most 16384 pages.
        static void* blocks[(64UL * 1024 * 1024) / PAGE_SIZE];
        size_t       count      = 0;
        uint64_t     before     = DmaFreePages();
        uint64_t     total      = 0;
        uint64_t     freeBefore = 0;
        uint64_t     freeAfter  = 0;
        resources->physicalMemoryManager->GetMemoryInfo(&total, &freeBefore);

        for (size_t order = MAX_ORDER + 1; order != 0; order--)
        {
            while (count < sizeof(blocks) / sizeof(blocks[0]))
            {
                void* block = pmm_alloc_dma(PAGE_SIZE * (1UL << (order - 1)), UINT32_MAX, nullptr);

                if (block == nullptr)
                {
                    break;
                }

                blocks[count++] = block;
            }
        }

        DmaAllocation buffer {};
        t.Equal(DmaFreePages(), 0, "dedicated zone exhausted");
        t.Check(testManager.AllocateCoherent(device, PAGE_SIZE, 1, 0, &buffer) == DmaStatus::NoMemory, "DMA exhaustion has no Normal fallback");
        void* ordinary = pmm_alloc(PAGE_SIZE);
        t.Check(ordinary != nullptr, "Normal allocation still works with DMA zone exhausted");
        pmm_free(ordinary);

        while (count != 0)
        {
            pmm_free(blocks[--count]);
            blocks[count] = nullptr;
        }

        t.Equal(DmaFreePages(), before, "zone exhaustion cleanup restores pages");

        for (size_t cycle = 0; cycle < 64; cycle++)
        {
            if (!t.Check(testManager.AllocateCoherent(device, PAGE_SIZE + cycle, 64, 65536, &buffer) == DmaStatus::Success, "repeated coherent allocation"))
            {
                break;
            }

            t.Check(testManager.FreeCoherent(&buffer) == DmaStatus::Success, "repeated coherent release");
        }

        resources->physicalMemoryManager->GetMemoryInfo(&total, &freeAfter);
        t.Equal(freeAfter, freeBefore, "cycles and failure restore total PMM accounting");
        t.Equal(DmaFreePages(), before, "cycles restore DMA zone accounting");
        t.Equal(testManager.CountAllocations(), 0, "cycles leave no tracking metadata");
    });
}
