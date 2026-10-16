#include "poststartkerneltests.hpp"
#include "integrationtests.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
#include <arch/arch.h>
#include <klib/klib.h>
#include <platform.h>
#include <selftests/SelfTestManager.hpp>

void run_poststartkernel_selftest_group(void* resourceLayerCaps, void* logicLayerCaps, bool vfsMounted)
{
    const bool       isBsp   = arch_cpu_id() == platform.bsp_id;
    SelfTestManager& manager = SelfTestManager::Get();

    if (isBsp)
    {
        manager.GroupBegin("poststartkernel");
    }

    if (!vfsMounted)
    {
        // Every CPU takes this branch; do not wait for workers we did not run.
        if (isBsp)
        {
            manager.CaseBegin("poststartkernel_vfs_mount");
            manager.RecordFailureDetail("root VFS mount failed; poststartkernel tests skipped");
            manager.CaseEnd("poststartkernel_vfs_mount", 0, 1);
            manager.GroupEnd("poststartkernel");
        }

        return;
    }

    if (isBsp)
    {
        manager.CaseBegin("poststartkernel/smp/scheduler_and_filesystem");
    }

    smp_selftests();

    if (!isBsp)
    {
        test_tlb_integration(static_cast<ResourceLayerCaps*>(resourceLayerCaps));
        return;
    }

    // SMP and mounted VFS fixtures modify the same file and create processes
    // against the shared kernel address space. ELF and syscall fixtures also
    // create user mappings there. Finish all SMP workers before these fixtures.
    if (!smp_selftests_wait_for_all_cpus())
    {
        manager.AbortUnsafe("SMP workers exceeded bounded completion wait");
    }

    if (!smp_selftests_cleanup_safe())
    {
        manager.AbortUnsafe("SMP fixture cleanup failed");
    }

    unsigned long long smpPasses   = 0;
    unsigned long long smpFails    = 0;
    unsigned long long smpFinished = 0;
    smp_selftests_get_totals(&smpPasses, &smpFails, &smpFinished);
    KDEBUG("smp summary: finished_cpus=%llu pass=%llu fail=%llu\n", smpFinished, smpPasses, smpFails);
    smp_selftests_kdebug_summary_details();
    if (smpFails > 0)
    {
        smp_selftests_record_failure_details();
    }

    manager.CaseEnd("poststartkernel/smp/scheduler_and_filesystem", smpPasses, smpFails);
    test_tlb_integration(static_cast<ResourceLayerCaps*>(resourceLayerCaps));
    run_poststart_vfs_selftests(resourceLayerCaps, logicLayerCaps);
    run_poststart_elf_selftests(resourceLayerCaps, logicLayerCaps);
    run_task_selftests(resourceLayerCaps);
    run_process_selftests(resourceLayerCaps, logicLayerCaps);
    auto* resources = static_cast<ResourceLayerCaps*>(resourceLayerCaps);
    auto* logic     = static_cast<LogicLayerCaps*>(logicLayerCaps);
    test_memory_integration(resources, logic);
    test_event_integration(resources, logic);
    test_wait_integration(resources, logic);
    test_signal_integration(resources, logic);
    test_communication_integration(resources, logic);
    manager.GroupEnd("poststartkernel");
}
