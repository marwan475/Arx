#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/VirtualMemoryManager.hpp"

void test_VirtualMemoryManager(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->virtualMemoryManager : nullptr;
    ManagerSuite suite("resource/VirtualMemoryManager", m != nullptr);
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
