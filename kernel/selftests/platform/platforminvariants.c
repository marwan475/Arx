#include <selftests/selftests.h>
#include <klib/klib.h>
#include <platform.h>
#include <memory/vmm.h>

void run_platform_invariants(void)
{
    unsigned long long pass = 0, fail = 0;
    selftest_case_begin("platform/cpu/initialized_state");

    if (platform.cpus && platform.cpu_count && arch_cpu_id() < platform.cpu_count)
    {
        ++pass;
    }
    else
    {
        ++fail;
        selftest_record_failure_detail("initialized CPU inventory");
    }

    cpu_info_t* cpu = &platform.cpus[arch_cpu_id()];

    if (cpu->initialized && cpu->numa_node && cpu->address_space && cpu->address_space->pt == arch_get_pt())
    {
        ++pass;
    }
    else
    {
        ++fail;
        selftest_record_failure_detail("CPU, NUMA and active page table agree");
    }

    selftest_case_end("platform/cpu/initialized_state", pass, fail);
    pass = fail = 0;
    selftest_case_begin("platform/cpu/preemption_balance");
    uint32_t depth = cpu->preemption_depth;
    cpu_preemption_disable();

    if (cpu->preemption_depth == depth + 1)
    {
        ++pass;
    }
    else
    {
        ++fail;
        selftest_record_failure_detail("disable increments depth");
    }

    cpu_preemption_enable();

    if (cpu->preemption_depth == depth)
    {
        ++pass;
    }
    else
    {
        ++fail;
        selftest_record_failure_detail("enable restores depth");
    }

    selftest_case_end("platform/cpu/preemption_balance", pass, fail);
    selftest_case_begin("platform/terminal/boot_framebuffer");

    if (!platform.terminal_context)
    {
        kprintf("[SELFTEST] SKIP platform/terminal: no boot terminal available\n");
        selftest_case_finish("platform/terminal/boot_framebuffer", 0, 0, SELFTEST_SKIP);
    }
    else
    {
        selftest_case_end("platform/terminal/boot_framebuffer", 1, 0);
    }

    selftest_case_begin("platform/device/inventory");

    if (platform.pci_device_count == 0)
    {
        kprintf("[SELFTEST] SKIP platform/device: no PCI devices discovered\n");
        selftest_case_finish("platform/device/inventory", 0, 0, SELFTEST_SKIP);
    }
    else
    {
        selftest_case_end("platform/device/inventory", platform.pci_devices ? 1 : 0, platform.pci_devices ? 0 : 1);
    }
}
