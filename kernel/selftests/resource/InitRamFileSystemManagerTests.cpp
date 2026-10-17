#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/InitRamFileSystemManager.hpp"

void test_InitRamFileSystemManager(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->initRamFileSystemManager : nullptr;
    ManagerSuite suite("resource/InitRamFileSystemManager", m != nullptr);
    suite.Run("resource/InitRamFileSystemManager/enumeration_lookup", [&](TestCase& t)
    {
        size_t count = m->GetArchiveCount();

        if (!t.Check(count > 0 && count <= m->MAX_ARCHIVES, "boot archive contains bounded entries"))
        {
            return;
        }

        for (size_t i = 0; i < count; ++i)
        {
            auto* entry = m->GetArchiveAt(i);

            if (!t.Check(entry && entry->path, "archive entry has path"))
            {
                continue;
            }

            t.Check(m->find(entry->path) == entry, "enumeration and lookup agree");
            t.Check(entry->size == 0 || entry->data, "nonempty entry has backing");
        }
    });

    suite.Run("resource/InitRamFileSystemManager/missing_bounds", [&](TestCase& t)
    {
        t.Check(m->GetArchiveAt(m->GetArchiveCount()) == nullptr, "past-last index");
        t.Check(m->GetArchiveAt(SIZE_MAX) == nullptr, "overflow index");
        t.Check(m->find(".__arx_selftest_missing__") == nullptr, "absent path");
    });
}
