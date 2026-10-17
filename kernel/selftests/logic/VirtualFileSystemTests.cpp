#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>
#include <selftests/MemoryFileFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"

void test_VirtualFileSystem(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->virtualFileSystem : nullptr;
    ManagerSuite suite("logic/VirtualFileSystem", m != nullptr);
    suite.Run("logic/VirtualFileSystem/mapping_prepare_ownership", [&](TestCase& t)
    {
        if (!resources || !resources->virtualMemoryManager)
        {
            t.Block("resource VM unavailable");
            return;
        }

        MemoryFileFixture file(m, 64);
        auto* backing = resources->virtualMemoryManager->CreateBacking(64, true);

        if (!t.Check(backing != nullptr, "create callback backing"))
        {
            return;
        }

        file.inode.backing = backing;
        file.operations.GetBacking = [](file_t* input, VirtualMemoryManager*, uint32_t, bool, virt_backing_object_t** output, uint32_t* maximum) -> int64_t
        {
            *output = input->inode->backing;
            *maximum = 7; // VFS must still enforce the descriptor's access ceiling.

            return 0;
        };
        virt_backing_object_t* prepared = nullptr;
        uint32_t               maximum  = 0;
        t.Equal(m->AcquireMappingBacking(&file.file, 0, PAGE_SIZE, 3, true, &prepared, &maximum), LINUX_EOPNOTSUPP, "shared external storage requires a writer");
        t.Check(prepared == nullptr && backing->references == 1 && backing->mappings == 0, "failed preparation transfers no borrowed ownership");
        t.Equal(m->AcquireMappingBacking(&file.file, 0, PAGE_SIZE, 3, false, &prepared, &maximum), 0, "private file backing needs no writer");

        if (prepared)
        {
            m->ReleaseMappingBacking(prepared, false, maximum);
        }

        file.file.statusFlags = 0;
        prepared              = nullptr;
        t.Equal(m->AcquireMappingBacking(&file.file, 0, PAGE_SIZE, 1, true, &prepared, &maximum), 0, "read-only descriptor permits shared read mapping");
        t.Check(maximum == 5 && backing->writable_mappings == 0, "callback cannot elevate descriptor permissions");

        if (prepared)
        {
            m->ReleaseMappingBacking(prepared, true, maximum);
        }

        t.Check(backing->references == 1 && backing->mappings == 0, "preparation reservations balance");
        file.inode.backing = nullptr;
        vmm_put_object(backing);
    });
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
