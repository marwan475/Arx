#include "integrationtests.hpp"

#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/ProcessController.hpp"

void test_memory_integration(ResourceLayerCaps* r, LogicLayerCaps* l)
{
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
