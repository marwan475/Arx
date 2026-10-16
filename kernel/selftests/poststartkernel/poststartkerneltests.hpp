#pragma once

// Called from every CPU's post-init task with the same published mount state.
// APs only run SMP workers; the BSP waits, records, and runs mounted fixtures.
void run_poststartkernel_selftest_group(void* resourceLayerCaps, void* logicLayerCaps, bool vfsMounted);
extern "C"
{
    void run_poststart_vfs_selftests(void* resourceLayerCaps, void* logicLayerCaps);
    void run_poststart_elf_selftests(void* resourceLayerCaps, void* logicLayerCaps);
    void run_smp_scheduler_selftest(void* logicLayerCaps, void* resourceLayerCaps, unsigned long long cpuId, unsigned long long* outPasses, unsigned long long* outFails, unsigned long long* outFailMask);
    void smp_selftests(void);
    bool smp_selftests_wait_for_all_cpus(void);
    bool smp_selftests_cleanup_safe(void);
    void smp_selftests_get_totals(unsigned long long* out_passes, unsigned long long* out_fails, unsigned long long* out_finished_cpus);
    void smp_selftests_kdebug_summary_details(void);
    void smp_selftests_record_failure_details(void);
}
