#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/VirtualMemoryManager.hpp"
#include "layers/DispatcherRoutes.hpp"

namespace
{
struct BackingRouteFixture
{
    unsigned writes;
    unsigned releases;
    bool     rejectWrite;
    bool     validWrite;
};

int64_t write_backing_fixture(void* context, uint64_t offset, const void* bytes, uint64_t size)
{
    auto* fixture = static_cast<BackingRouteFixture*>(context);
    ++fixture->writes;
    fixture->validWrite = offset == 0 && size == PAGE_SIZE && static_cast<const uint8_t*>(bytes)[0] == 0x5a;

    return fixture->rejectWrite ? -5 : static_cast<int64_t>(size);
}

void release_backing_fixture(void* context)
{
    auto* fixture = static_cast<BackingRouteFixture*>(context);
    ++fixture->releases;
}
} // namespace

void test_VirtualMemoryManager(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->virtualMemoryManager : nullptr;
    ManagerSuite suite("resource/VirtualMemoryManager", m != nullptr);
    suite.Run("resource/VirtualMemoryManager/dispatcher_backing_retry", [&](TestCase& t)
    {
        BackingRouteFixture                   fixture = {};
        const dispatcher_backing_operations_t route(write_backing_fixture, release_backing_fixture);
        auto*                                 object = m->CreateBacking(PAGE_SIZE, true);

        if (!t.Check(object != nullptr, "allocate routed file backing"))
        {
            return;
        }

        object->operations = &route;
        object->context    = &fixture;
        auto* page         = m->GetBackingPage(object, 0);

        if (!t.Check(page != nullptr, "allocate routed backing page"))
        {
            m->ReleaseBacking(object);
            return;
        }

        static_cast<uint8_t*>(page->pointer)[0] = 0x5a;
        page->dirty                             = true;
        fixture.rejectWrite                     = true;

        m->ReleaseBacking(object);
        t.Equal(fixture.writes, 1U, "last-reference writeback reaches its provider");
        t.Equal(fixture.releases, 0U, "failed writeback preserves the provider context");

        fixture.rejectWrite = false;
        t.Equal(m->RetryWriteback(), 0, "Dispatcher and Request preserve retry completion");
        t.Equal(fixture.writes, 2U, "retry writes the retained dirty page once");
        t.Check(fixture.validWrite, "routing preserves the byte range and data");
        t.Equal(fixture.releases, 1U, "successful retry releases the context exactly once");

        if (fixture.releases != 1)
        {
            SelfTestManager::Get().AbortUnsafe("pending backing retained a stack-owned route fixture");
        }
    });

    suite.Run("resource/VirtualMemoryManager/shared_resident_accounting", [&](TestCase& t)
    {
        const uint64_t before = vmm_shared_memory_bytes();
        auto* object = m->CreateBacking(PAGE_SIZE);

        if (!t.Check(object != nullptr, "allocate anonymous shared object"))
        {
            return;
        }

        auto* page = m->GetBackingPage(object, 0);

        if (!t.Check(page != nullptr, "allocate resident shared page"))
        {
            m->ReleaseBacking(object);
            return;
        }

        t.Equal(vmm_shared_memory_bytes(), before + PAGE_SIZE, "resident shared memory counts actual pages");
        t.Check(m->GetBackingPage(object, 0) == page, "reusing an alias returns the same backing");
        t.Equal(vmm_shared_memory_bytes(), before + PAGE_SIZE, "aliases never double count physical memory");
        __atomic_add_fetch(&page->references, 1U, __ATOMIC_RELAXED);
        m->ReleaseBacking(object);
        t.Equal(vmm_shared_memory_bytes(), before + PAGE_SIZE, "independent page owner keeps resident shared memory charged");
        vmm_put_page(page);
        t.Equal(vmm_shared_memory_bytes(), before, "last page owner releases shared memory accounting");
    });

    suite.Run("resource/VirtualMemoryManager/address_space_lifecycle", [&](TestCase& t)
    {
        auto* space = m->CreateUserAddressSpace();

        if (!t.Check(space != nullptr, "create isolated user root"))
        {
            return;
        }

        t.Check(space->pt != 0 && space->type == VIRT_ADDR_USER, "valid isolated page table");
        t.Equal(m->VirtToPhys(0x40000000, space), 0, "new user address is unmapped");
        m->DestroyUserAddressSpace(space);
    });

    suite.Run("resource/VirtualMemoryManager/anonymous_page", [&](TestCase& t)
    {
        auto* space = m->CreateUserAddressSpace();

        if (!t.Check(space != nullptr, "create address-space fixture"))
        {
            return;
        }

        const uint64_t address = 0x40000000;
        spinlock_acquire_preempt(&space->operation_lock);
        bool allocated = m->AllocateAnonymousPage(space, address, 3, false);
        t.Check(allocated, "allocate anonymous page");

        if (allocated)
        {
            t.Check(m->VirtToPhys(address, space) != 0, "anonymous page translates");
            t.Check(!m->IsPageAvailable(space, address), "allocated page cannot be reserved again");
            t.Check(!m->AllocateAnonymousPage(space, address, 3, false), "duplicate allocation rejected");
            m->ProtectAnonymousPage(space, address, 1);
            t.Equal(m->FindRegion(space, address)->protection, 1, "protection metadata");
            m->DiscardAnonymousPage(space, address);
            t.Check(m->FindRegion(space, address)->backing != nullptr, "discard preserves reservation");
            m->ReleaseAnonymousPage(space, address);
            t.Equal(m->VirtToPhys(address, space), 0, "release removes translation");
            t.Check(m->IsPageAvailable(space, address), "release returns address to free set");
        }

        spinlock_release_preempt(&space->operation_lock);
        m->DestroyUserAddressSpace(space);
    });

    suite.Run("resource/VirtualMemoryManager/clone_isolation", [&](TestCase& t)
    {
        auto* space = m->CreateUserAddressSpace();

        if (!t.Check(space != nullptr, "create source root"))
        {
            return;
        }

        spinlock_acquire_preempt(&space->operation_lock);
        bool allocated = m->AllocateAnonymousPage(space, 0x40000000, 3, false);
        spinlock_release_preempt(&space->operation_lock);

        if (t.Check(allocated, "create clone source page"))
        {
            auto* clone = m->CloneUserAddressSpace(space);

            if (t.Check(clone != nullptr, "clone user address space"))
            {
                t.Check(clone->pt != space->pt, "clone has distinct root");
                t.Equal(m->VirtToPhys(0x40000000, clone), m->VirtToPhys(0x40000000, space), "clone initially shares backing");
                m->DestroyUserAddressSpace(clone);
                t.Check(m->VirtToPhys(0x40000000, space) != 0, "destroying clone preserves source");
            }
        }

        m->DestroyUserAddressSpace(space);
    });

    suite.Run("resource/VirtualMemoryManager/region_reservation", [&](TestCase& t)
    {
        auto* space = m->CreateUserAddressSpace();

        if (!t.Check(space != nullptr, "create region fixture"))
        {
            return;
        }

        virt_addr_t address = m->ReserveRegion(space, PAGE_SIZE + 1, VIRT_ADDR_USER);

        if (t.Check(address != 0, "reserve user range"))
        {
            auto* region = m->FindRegion(space, address);

            if (t.Check(region != nullptr, "find reservation"))
            {
                t.Equal(region->size, PAGE_SIZE * 2, "reservation rounds to pages");
                t.Equal(region->type, VIRT_ADDR_USER, "reservation type");
            }

            m->FreeRegion(space, address);
            t.Check(m->FindRegion(space, address) == nullptr, "release removes reservation");
        }

        m->DestroyUserAddressSpace(space);
    });
}
