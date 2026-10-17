#include <selftests/selftests.h>
#include <klib/klib.h>
#include <platform.h>
#include <memory/vmm.h>
#include <memory/numa.h>
#include <uacpi/acpi.h>

void test_numa_topology(void);

void run_platform_invariants(void)
{
    test_numa_topology();
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

static void fix_table_checksum(struct acpi_sdt_hdr* header)
{
    header->checksum = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < header->length; ++i)
    {
        sum += ((uint8_t*) header)[i];
    }
    header->checksum = (uint8_t) -sum;
}

void test_numa_topology(void)
{
    struct
    {
        struct acpi_srat table;
        struct acpi_srat_x2apic_affinity cpus[2];
        struct acpi_srat_memory_affinity memory[2];
    } fixture = {0};
    memcpy(fixture.table.hdr.signature, "SRAT", 4);
    fixture.table.hdr.length = sizeof(fixture);
    fixture.table.hdr.revision = 3;
    uint32_t ids[] = {17, 42};
    for (size_t i = 0; i < 2; ++i)
    {
        fixture.cpus[i].hdr.type = 2;
        fixture.cpus[i].hdr.length = sizeof(fixture.cpus[i]);
        fixture.cpus[i].id = ids[i];
        fixture.cpus[i].proximity_domain = i == 0 ? 1234 : 9999;
        fixture.cpus[i].flags = 1;
        fixture.memory[i].hdr.type = 1;
        fixture.memory[i].hdr.length = sizeof(fixture.memory[i]);
        fixture.memory[i].proximity_domain = 1234;
        fixture.memory[i].flags = 1;
        fixture.memory[i].address = (i + 1) * 0x1000000ULL;
        fixture.memory[i].length = 0x1000000;
    }
    fix_table_checksum(&fixture.table.hdr);
    numa_topology_t topology;
    unsigned long long passed = 0, failed = 0;
    selftest_case_begin("platform/memory/numa_topology");
    if (numa_parse_srat(&fixture, sizeof(fixture), ids, 2, &topology) && topology.node_count == 2 && topology.cpu_nodes[0] == 0 && topology.cpu_nodes[1] == 1 && topology.ranges[1].node == 0)
    {
        ++passed;
    }
    else
    {
        ++failed;
        selftest_record_failure_detail("sparse proximity domains become dense CPU nodes, including memoryless nodes");
    }
    fixture.memory[1].address = fixture.memory[0].address;
    fix_table_checksum(&fixture.table.hdr);
    if (!numa_parse_srat(&fixture, sizeof(fixture), ids, 2, &topology))
    {
        ++passed;
    }
    else
    {
        ++failed;
        selftest_record_failure_detail("overlapping memory ownership rejected");
    }
    fixture.memory[1].address = 0x2000000;
    fixture.cpus[1].hdr.length = 0;
    fix_table_checksum(&fixture.table.hdr);
    if (!numa_parse_srat(&fixture, sizeof(fixture), ids, 2, &topology))
    {
        ++passed;
    }
    else
    {
        ++failed;
        selftest_record_failure_detail("malformed subtable rejected without looping");
    }
    fixture.cpus[1].hdr.length = sizeof(fixture.cpus[1]);
    fix_table_checksum(&fixture.table.hdr);
    fixture.table.hdr.checksum ^= 1;
    if (!numa_parse_srat(&fixture, sizeof(fixture), ids, 2, &topology))
    {
        ++passed;
    }
    else
    {
        ++failed;
        selftest_record_failure_detail("invalid SRAT checksum rejected");
    }
    selftest_case_end("platform/memory/numa_topology", passed, failed);
}
