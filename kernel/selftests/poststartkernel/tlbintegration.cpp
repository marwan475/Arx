#include "integrationtests.hpp"

#include <selftests/TestCase.hpp>
#include <selftests/PollBudget.hpp>

#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <platform.h>

namespace
{
struct coherence_control_t
{
    uint32_t command;
    uint32_t ready[BOOT_SMP_MAX_CPUS];
    uint32_t acknowledged[BOOT_SMP_MAX_CPUS];
    uint64_t observed[BOOT_SMP_MAX_CPUS];
    uint64_t address;
    bool     unsafe;
};
coherence_control_t control;
bool wait_for(uint32_t command, bool ready)
{
    PollBudget budget;

    for (;;)
    {
        bool complete = true;

        for (size_t cpu = 0; cpu < platform.cpu_count; ++cpu)
        {
            if (cpu == platform.bsp_id)
            {
                continue;
            }

            uint32_t state = __atomic_load_n(ready ? &control.ready[cpu] : &control.acknowledged[cpu], __ATOMIC_ACQUIRE);

            if (state != command)
            {
                complete = false;
            }
        }

        if (__atomic_load_n(&control.unsafe, __ATOMIC_ACQUIRE))
        {
            return false;
        }

        if (complete)
        {
            return true;
        }

        if (budget.Expired())
        {
            return false;
        }

        arch_pause();
    }
}

void stop_peers()
{
    __atomic_store_n(&control.command, 3U, __ATOMIC_RELEASE);

    if (!wait_for(3, false))
    {
        SelfTestManager::Get().AbortUnsafe("TLB test peers did not quiesce");
    }
}
}

// All CPUs enter once; APs stay available for remote invalidation requests until
// the BSP explicitly releases them. No user mappings or files are shared here.
void test_tlb_integration(ResourceLayerCaps* r)
{
    const uint8_t cpu = arch_cpu_id();

    if (cpu != platform.bsp_id)
    {
        __atomic_store_n(&control.ready[cpu], 1U, __ATOMIC_RELEASE);
        uint32_t   previous = 0;
        PollBudget budget;

        for (;;)
        {
            uint32_t command = __atomic_load_n(&control.command, __ATOMIC_ACQUIRE);

            if (command != previous && command != 0)
            {
                if (command != 3)
                {
                    control.observed[cpu] = *reinterpret_cast<volatile uint64_t*>(control.address);
                }

                __atomic_store_n(&control.acknowledged[cpu], command, __ATOMIC_RELEASE);

                if (command == 3)
                {
                    return;
                }

                previous = command;
            }

            if (budget.Expired())
            {
                __atomic_store_n(&control.unsafe, true, __ATOMIC_RELEASE);
                return;
            }

            arch_pause();
        }
    }

    TestCase t("poststartkernel/memory/cross_cpu_tlb_remap");

    if (!wait_for(1, true))
    {
        SelfTestManager::Get().AbortUnsafe("TLB test peers did not arrive");
    }

    if (platform.cpu_count < 2)
    {
        t.Skip("requires at least two CPUs");
        stop_peers();
        return;
    }

    if (!r || !r->virtualMemoryManager || !r->physicalMemoryManager)
    {
        t.Block("memory managers unavailable");
        stop_peers();
        return;
    }

    auto* space = platform.cpus[cpu].address_space;

    for (size_t other = 0; other < platform.cpu_count; ++other)
    {
        if (platform.cpus[other].address_space != space)
        {
            t.Block("post-init CPUs do not share the kernel root");
            stop_peers();
            return;
        }
    }

    auto*       vm      = r->virtualMemoryManager;
    auto*       pm      = r->physicalMemoryManager;
    virt_addr_t address = vm->ReserveRegion(space, PAGE_SIZE, VIRT_ADDR_KERNEL);
    void*       first   = pm->Alloc(PAGE_SIZE);
    void*       second  = pm->Alloc(PAGE_SIZE);
    bool        mapped  = false;

    if (t.Check(address && first && second, "reserve remap fixture pages"))
    {
        *static_cast<uint64_t*>(first) = 0x11223344;
        *static_cast<uint64_t*>(second) = 0x55667788;
        auto&    zone  = platform.numa_nodes[0].zone;
        uint64_t flags = 0;
        ARCH_PAGE_FLAGS_INIT(flags);
        ARCH_PAGE_FLAG_SET_READ(flags);
        ARCH_PAGE_FLAG_SET_WRITE(flags);
        mapped = vm->MapPage(address, hhdm_to_pa(reinterpret_cast<uintptr_t>(first), zone.hhdm_present, zone.hhdm_offset), flags, space);

        if (t.Check(mapped, "install first shared mapping"))
        {
            control.address = address;
            __atomic_store_n(&control.command, 1U, __ATOMIC_RELEASE);

            if (!wait_for(1, false))
            {
                SelfTestManager::Get().AbortUnsafe("initial TLB read did not complete");
            }

            for (size_t other = 0; other < platform.cpu_count; ++other)
            {
                if (other != cpu)
                {
                    t.Equal(control.observed[other], 0x11223344, "peer populated initial translation");
                }
            }

            vm->UnmapPage(address, space);
            mapped = vm->MapPage(address, hhdm_to_pa(reinterpret_cast<uintptr_t>(second), zone.hhdm_present, zone.hhdm_offset), flags, space);

            if (t.Check(mapped, "install replacement shared mapping"))
            {
                __atomic_store_n(&control.command, 2U, __ATOMIC_RELEASE);

                if (!wait_for(2, false))
                {
                    SelfTestManager::Get().AbortUnsafe("remapped TLB read did not complete");
                }

                for (size_t other = 0; other < platform.cpu_count; ++other)
                {
                    if (other != cpu)
                    {
                        t.Equal(control.observed[other], 0x55667788, "peer observes replacement physical page");
                    }
                }
            }
        }
    }

    stop_peers();

    if (mapped)
    {
        vm->UnmapPage(address, space);

        if (vm->VirtToPhys(address, space) != 0)
        {
            SelfTestManager::Get().AbortUnsafe("TLB fixture mapping survived cleanup");
        }
    }

    if (address)
    {
        vm->FreeRegion(space, address);
    }

    if (first)
    {
        pm->Free(first);
    }

    if (second)
    {
        pm->Free(second);
    }
}
