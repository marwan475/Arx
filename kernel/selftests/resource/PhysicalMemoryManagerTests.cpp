#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/PhysicalMemoryManager.hpp"

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
}
