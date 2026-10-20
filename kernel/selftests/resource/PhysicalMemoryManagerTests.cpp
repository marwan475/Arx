#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/PhysicalMemoryManager.hpp"
#include <platform.h>

void test_PhysicalMemoryManager(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->physicalMemoryManager : nullptr;
    ManagerSuite suite("resource/PhysicalMemoryManager", m != nullptr);
    suite.Run("resource/PhysicalMemoryManager/accounting", [&](TestCase& t)
    {
        uint64_t total = 0;
        uint64_t free = 0;
        m->GetMemoryInfo(&total, &free);
        t.Check(total > 0 && free <= total, "physical accounting is bounded by total memory");
        t.Equal(free % PAGE_SIZE, 0, "free memory is page granular");
    });

    suite.Run("resource/PhysicalMemoryManager/allocate_free", [&](TestCase& t)
    {
        uint64_t total;
        uint64_t before;
        uint64_t after;
        m->GetMemoryInfo(&total, &before);
        void* p = m->Alloc(PAGE_SIZE * 2);

        if (!t.Check(p != nullptr, "allocate two contiguous pages"))
        {
            return;
        }

        t.Equal(reinterpret_cast<uintptr_t>(p) % PAGE_SIZE, 0, "allocation alignment");
        memset(p, 0x5a, PAGE_SIZE * 2);
        t.Equal(static_cast<uint8_t*>(p)[PAGE_SIZE * 2 - 1], 0x5a, "allocation includes last byte");
        m->Free(p);
        m->GetMemoryInfo(&total, &after);
        t.Equal(after, before, "free restores allocation accounting");
    });
#if DEBUG
    suite.Run("resource/PhysicalMemoryManager/debug_counters", [&](TestCase& t)
    {
        const arx_allocation_stats_t before = platform.debugPmm.requests[0];
        void* pointer = m->Alloc(PAGE_SIZE + 1);

        if (!t.Check(pointer != nullptr, "rounded debug allocation"))
        {
            return;
        }

        const arx_allocation_stats_t during = platform.debugPmm.requests[0];
        t.Equal(during.attempts, before.attempts + 1, "one public request counted");
        t.Equal(during.successes, before.successes + 1, "one successful block counted");
        t.Equal(during.liveBlocks, before.liveBlocks + 1, "block count is independent of page count");
        t.Equal(during.liveRoundedBytes, before.liveRoundedBytes + PAGE_SIZE * 2, "live bytes use buddy rounding");
        t.Equal(during.requestedBytes, before.requestedBytes + PAGE_SIZE + 1, "requested totals preserve original bytes");
        t.Equal(during.roundedBytes, before.roundedBytes + PAGE_SIZE * 2, "committed totals preserve rounded bytes");
        m->Free(pointer);
        t.Equal(platform.debugPmm.requests[0].liveBlocks, before.liveBlocks, "free restores live blocks");
        t.Equal(platform.debugPmm.requests[0].liveRoundedBytes, before.liveRoundedBytes, "free restores live rounded bytes");
        t.Equal(platform.debugPmm.requests[0].frees, before.frees + 1, "accepted free counted once");
        t.Check(m->Alloc(0) == nullptr, "zero request fails");
        t.Equal(platform.debugPmm.requests[0].failures, before.failures + 1, "terminal request failure counted once");

        for (size_t node = 0; node < platform.numa_node_count; node++)
        {
            zone_t* zones[] = { &platform.numa_nodes[node].zone, &platform.numa_nodes[node].dma_zone };

            for (zone_t* zone : zones)
            {
                uint64_t observedPages = 0;

                for (size_t order = 0; order <= MAX_ORDER; order++)
                {
                    uint64_t blocks = 0;

                    for (page_t* block = zone->buddy_free_lists[order].head; block != nullptr; block = block->next)
                    {
                        if (++blocks > zone->total_pages)
                        {
                            break;
                        }
                    }

                    t.Equal(zone->debugStats.freeBlocks[order], blocks, "maintained per-order blocks match buddy lists");
                    observedPages += blocks << order;
                }

                t.Equal(observedPages, zone->free_pages, "per-order counts cover free pages");
                t.Equal(zone->debugStats.group.activeWriters, 0, "zone update groups completed");
            }
        }
    });
#endif

}
