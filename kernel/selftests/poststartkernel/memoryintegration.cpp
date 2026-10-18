#include "integrationtests.hpp"

#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/ProcessController.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/InitRamFileSystemManager.hpp"

void test_memory_integration(ResourceLayerCaps* r, LogicLayerCaps* l)
{
    {
        TestCase t("poststartkernel/memory/file_shared_io_writeback");

        if (!l || !l->virtualFileSystem || !l->processController || !r || !r->initRamFileSystemManager)
        {
            t.Block("VFS, process memory or initramfs unavailable");
            return;
        }

        TestFixture f(r);

        if (!f.Valid(t))
        {
            return;
        }

        auto* vfs    = l->virtualFileSystem;
        auto* file   = vfs->Open({}, "/test.txt", 2);
        auto* second = vfs->Open({}, "/test.txt", 2);

        if (!t.Check(file && second, "open same file twice"))
        {
            if (file)
            {
                vfs->Close(file);
            }

            if (second)
            {
                vfs->Close(second);
            }

            return;
        }

        const int64_t fd = r->processManager->AddFileDescriptor(f.Process, file, 0, dispatcher_vfs_file_operations());

        if (!t.Check(fd >= 0, "install mapping descriptor"))
        {
            vfs->Close(file);
            vfs->Close(second);
            return;
        }

        uint8_t original = 0;
        t.Equal(vfs->ReadAt(second, &original, 1, 0), 1, "capture original file byte");
        const uint64_t address = l->processController->MapProcessMemory(0x50000000, PAGE_SIZE, 3, 1, fd, 0);

        if (t.Equal(address, 0x50000000, "map shared file through VFS callback"))
        {
            uint8_t value = original ^ 0x5a, observed = 0;
            t.Check(safe_write_user(address, &value, 1), "mapped store");
            t.Check(vfs->ReadAt(second, &observed, 1, 0) == 1 && observed == value, "separate open reads mapped bytes");
            value ^= 0x33;
            t.Check(vfs->WriteAt(second, &value, 1, 0) == 1 && safe_read_user(address, &observed, 1) && observed == value, "separate open write reaches mapping");
            t.Equal(vfs->TruncateFile(second, 0), LINUX_EOPNOTSUPP, "active mapped shrink rejected");
            t.Check(f.Close(fd), "close mapping descriptor");
            t.Check(safe_write_user(address, &original, 1), "restore original byte through surviving mapping");
            t.Equal(l->processController->UnmapProcessMemory(address, PAGE_SIZE), 0, "unmap flushes shared file");
            auto* archive = r->initRamFileSystemManager->find("test.txt");
            t.Check(archive && archive->size && archive->data[0] == original, "writeback reaches resource file data");
        }
        else
        {
            f.Close(fd);
        }

        vfs->Close(second);
    }

    {
        TestCase t("poststartkernel/memory/user_copy_and_protection");

        if (!l || !l->processController)
        {
            t.Block("process controller unavailable");
            return;
        }

        TestFixture f(r);

        if (!f.Valid(t) || !t.Check(f.Map(), "map owned buffer"))
        {
            return;
        }

        uint64_t expected = 0x123456789abcdef0;

        uint64_t actual = 0;
        t.Check(safe_write_user(TestFixture::Buffer, &expected, sizeof(expected)), "write through real user-copy validation");
        t.Check(safe_read_user(TestFixture::Buffer, &actual, sizeof(actual)), "read mapped user bytes");
        t.Equal(actual, expected, "user copy preserves value");
        t.Equal(l->processController->ProtectProcessMemory(TestFixture::Buffer, PAGE_SIZE, 1), 0, "make buffer read-only");
        t.Check(!safe_write_user(TestFixture::Buffer, &expected, sizeof(expected)), "user copy rejects read-only target");
        t.Equal(l->processController->UnmapProcessMemory(TestFixture::Buffer, PAGE_SIZE), 0, "unmap buffer");
        t.Check(!safe_read_user(TestFixture::Buffer, &actual, sizeof(actual)), "user copy rejects unmapped source");
    }

    {
        TestCase t("poststartkernel/memory/copy_on_write");
        TestFixture f(r);

        if (!f.Valid(t) || !t.Check(f.Map(), "map COW source"))
        {
            return;
        }

        uint64_t before = 17;

        uint64_t after = 29;
        t.Check(safe_write_user(TestFixture::Buffer, &before, 8), "seed COW source");
        auto* clone = r->virtualMemoryManager->CloneUserAddressSpace(f.Space);

        if (!t.Check(clone != nullptr, "clone source"))
        {
            return;
        }

        phys_addr_t original = r->virtualMemoryManager->VirtToPhys(TestFixture::Buffer, clone);
        t.Check(safe_write_user(TestFixture::Buffer, &after, 8), "write resolves COW through user-copy path");
        t.Check(original != r->virtualMemoryManager->VirtToPhys(TestFixture::Buffer, f.Space), "writer receives private backing");
        auto&           zone   = platform.numa_nodes[0].zone;
        const uint64_t* cloned = reinterpret_cast<const uint64_t*>(pa_to_hhdm(original, zone.hhdm_present, zone.hhdm_offset));
        t.Equal(*cloned, before, "clone retains original data");
        r->virtualMemoryManager->DestroyUserAddressSpace(clone);
    }
}
