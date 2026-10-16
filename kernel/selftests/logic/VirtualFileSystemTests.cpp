#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>
#include <selftests/MemoryFileFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"

void test_VirtualFileSystem(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->virtualFileSystem : nullptr;
    ManagerSuite suite("logic/VirtualFileSystem", m != nullptr);
    suite.Run("logic/VirtualFileSystem/path_join", [&](TestCase& t)
    {
        char path[64] = {};
        t.Check(m->JoinPath("/alpha", "beta", path, sizeof(path)), "join relative path");
        t.Check(strcmp(path, "/alpha/beta") == 0, "relative join result");
        t.Check(m->JoinPath("/ignored", "/absolute", path, sizeof(path)), "absolute path overrides base");
        t.Check(strcmp(path, "/absolute") == 0, "absolute join result");
        t.Check(!m->JoinPath("/", "long", path, 2), "short destination rejected");
        t.Check(!m->JoinPath(nullptr, "x", path, sizeof(path)), "null base rejected");
    });

    suite.Run("logic/VirtualFileSystem/anonymous_inode", [&](TestCase& t)
    {
        inode_t inode = {};
        m->InitializeAnonymousInode(&inode, INODE_REGULAR, 0600);
        t.Equal(inode.type, INODE_REGULAR, "anonymous inode type");
        t.Equal(inode.mode, 0600, "anonymous inode mode");
        t.Check(inode.metadataValid, "anonymous inode metadata initialized");
    });

    suite.Run("logic/VirtualFileSystem/file_reference", [&](TestCase& t)
    {
        file_t file   = {};
        file.refCount = 1;

        if (!t.Check(m->Retain(&file), "retain live reference"))
        {
            return;
        }

        t.Equal(file.refCount, 2, "reference increments");
        t.Equal(m->Close(&file), 0, "release nonfinal reference");
        t.Equal(file.refCount, 1, "reference decrements without freeing borrowed fixture");
        t.Check(!m->Retain(nullptr), "null retain rejected");
        t.Equal(m->Close(nullptr), (uint64_t) -1, "null close rejected");
    });

    suite.Run("logic/VirtualFileSystem/positioned_io", [&](TestCase& t)
    {
        MemoryFileFixture backend(m);
        const char        payload[] = "vfs-unit";
        char              actual[sizeof(payload)] = {};

        t.Equal(m->Write(&backend.file, payload, sizeof(payload)), sizeof(payload), "backend write dispatch");
        uint64_t offset = backend.file.offset;
        t.Equal(m->ReadAt(&backend.file, actual, sizeof(actual), 0), sizeof(actual), "positioned read dispatch");
        t.Check(memcmp(actual, payload, sizeof(payload)) == 0, "backend preserves bytes");
        t.Equal(backend.file.offset, offset, "positioned read preserves shared offset");
        t.Equal(m->Seek(&backend.file, 0, 0), 0, "seek dispatch");
        t.Equal(m->Read(&backend.file, actual, sizeof(actual)), sizeof(actual), "sequential read dispatch");
        t.Equal(m->Read(&backend.file, actual, sizeof(actual)), 0, "read reaches EOF");
        t.Equal(m->Seek(&backend.file, -1, 0), LINUX_EINVAL, "backend error propagation");
    });
}
