#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/ResourceFileSystem.hpp"
#include "layers/Resource/InitRamFileSystemManager.hpp"

void test_ResourceFileSystem(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->fileSystemCaps : nullptr;
    ManagerSuite suite("resource/ResourceFileSystem", m != nullptr);
    suite.Run("resource/ResourceFileSystem/backend_validation", [&](TestCase& t)
    {
        t.Check(m->MountFilesystem(m, "unsupported", nullptr) == nullptr, "unsupported filesystem rejected");
        resource_node_info_t info = {};
        t.Check(!m->GetNodeInfo(m, nullptr, &info), "null node rejected");
        t.Check(m->Lookup(m, nullptr, "missing") == nullptr, "lookup needs parent");
        char byte = 0;
        t.Equal(m->Read(m, nullptr, 0, &byte, 1), (uint64_t) -1, "read needs node");
    });

    suite.Run("resource/ResourceFileSystem/mutation_rejection", [&](TestCase& t)
    {
        t.Equal(m->Mkdir(m, "/.__unit__", 0755), (uint64_t) -30, "cpio mkdir is read-only");
        t.Equal(m->Unlink(m, "/.__unit__", false), (uint64_t) -30, "cpio unlink is read-only");
        t.Equal(m->Rename(m, "/a", "/b"), (uint64_t) -30, "cpio rename is read-only");
        t.Equal(m->Truncate(m, "/.__unit_missing__", 0), LINUX_ENOENT, "truncate missing entry");
    });

    suite.Run("resource/ResourceFileSystem/truncate_growth_boundary", [&](TestCase& t)
    {
        if (!resources->initRamFileSystemManager)
        {
            t.Block("archive manager unavailable");
            return;
        }

        auto* archive = resources->initRamFileSystemManager;

        for (size_t i = 0; i < archive->GetArchiveCount(); ++i)
        {
            auto* file = archive->GetArchiveAt(i);

            if (file && (file->mode & 0170000) == 0100000 && file->size < UINT64_MAX)
            {
                t.Equal(m->Truncate(m, file->path, file->size + 1), (uint64_t) -30, "archive entry cannot grow past backing");
                return;
            }
        }

        t.Block("no regular archive entry available");
    });
}
