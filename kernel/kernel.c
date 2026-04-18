#include <acpi/acpi.h>
#include <boot/boot.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <stdint.h>
#include <terminal/terminal.h>

void run_selftests(void);

dispatcher_t dispatcher;

// From bootloader we need
// - memory map
// - framebuffer info
// - smp info
// - all cores having same address space/pagetable
// - higher half direct map instead of identity mapping so user address space is separate from physical memory addresses
// - paging with no user access and RWX on direct map
// - acpi rsdp address
void kmain(struct boot_info* boot_info, uint64_t cpu_count)
{
    bool status = true;

    kprintf("Arx kernel: kmain entered\n");

    if (boot_info == 0 || boot_info->limine_present == 0)
    {
        kprintf("Arx kernel: no boot protocol info\n");
        panic();
    }

    if (boot_info->memmap_entries == 0 || boot_info->memmap_entry_count == 0)
    {
        kprintf("Arx kernel: no memory map entries\n");
        panic();
    }

    if (boot_info->memmap_entry_count > BOOT_MEMMAP_MAX_ENTRIES)
    {
        kprintf("Arx kernel: too many memory map entries (max %u)\n", BOOT_MEMMAP_MAX_ENTRIES);
        panic();
    }

    if (boot_info->framebuffer_addr == 0)
    {
        kprintf("Arx kernel: no framebuffer\n");
        panic();
    }

    kprintf("Arx kernel: framebuffer addr=0x%llx size=%llu x %llu pitch=%llu bpp=%llu\n", (unsigned long long) boot_info->framebuffer_addr, (unsigned long long) boot_info->framebuffer_width, (unsigned long long) boot_info->framebuffer_height, (unsigned long long) boot_info->framebuffer_pitch,
            (unsigned long long) boot_info->framebuffer_bpp);

    dispatcher.framebuffer.address          = (void*) (uintptr_t) boot_info->framebuffer_addr;
    dispatcher.framebuffer.width            = (size_t) boot_info->framebuffer_width;
    dispatcher.framebuffer.height           = (size_t) boot_info->framebuffer_height;
    dispatcher.framebuffer.pitch            = (size_t) boot_info->framebuffer_pitch;
    dispatcher.framebuffer.red_mask_size    = (uint8_t) boot_info->framebuffer_red_mask_size;
    dispatcher.framebuffer.red_mask_shift   = (uint8_t) boot_info->framebuffer_red_mask_shift;
    dispatcher.framebuffer.green_mask_size  = (uint8_t) boot_info->framebuffer_green_mask_size;
    dispatcher.framebuffer.green_mask_shift = (uint8_t) boot_info->framebuffer_green_mask_shift;
    dispatcher.framebuffer.blue_mask_size   = (uint8_t) boot_info->framebuffer_blue_mask_size;
    dispatcher.framebuffer.blue_mask_shift  = (uint8_t) boot_info->framebuffer_blue_mask_shift;

    if (!terminal_init(&dispatcher.framebuffer))
    {
        kprintf("Arx kernel: failed to initialize terminal\n");
        panic();
    }

    kterm_printf("Arx kernel: terminal initialized\n");

    debug_validate_boot(boot_info, cpu_count);

    KDEBUG("Arx debug: -> cpus_init(%llu)\n", (unsigned long long) cpu_count);
    cpus_init(cpu_count);
    KDEBUG("Arx debug: <- cpus_init done dispatcher.cpu_count=%llu\n", (unsigned long long) dispatcher.cpu_count);

    KDEBUG("Arx debug: -> pmm_init\n");
    pmm_init(boot_info);
    KDEBUG("Arx debug: <- pmm_init done\n");

    KDEBUG("Arx debug: -> vmm_init\n");
    vmm_init(boot_info);
    KDEBUG("Arx debug: <- vmm_init done\n");

    KDEBUG("Arx debug: -> acpi_init(rsdp=0x%llx)\n", (unsigned long long) boot_info->rsdp_address);
    acpi_init(boot_info->rsdp_address);
    KDEBUG("Arx debug: <- acpi_init done\n");

    KDEBUG("Arx debug: -> arch_init\n");
    status = arch_init();
    KDEBUG("Arx debug: <- arch_init done\n");

    if (!status)
    {
        kprintf("Arx kernel: architecture initialization failed\n");
        kterm_printf("Arx kernel: architecture initialization failed\n");
        panic();
    }

    arch_smp_init(boot_info);

    bool waiting_for_other_cpus = true;

    while (waiting_for_other_cpus)
    {
        for (size_t i = 0; i < dispatcher.cpu_count; i++)
        {
            if (!dispatcher.cpus[i].initialized)
            {
                waiting_for_other_cpus = true;
                break;
            }
            waiting_for_other_cpus = false;
        }
    }

    KDEBUG("Arx debug: -> run_selftests\n");
    run_selftests();
    KDEBUG("Arx debug: <- run_selftests done\n");

    kterm_printf("Arx kernel: initialization complete\n");

    for (;;)
    {
    }
}

void smp_kmain(void)
{
    kprintf("Arx kernel: cpu %d entered smp_kmain\n", arch_cpu_id());

    arch_init();

    kterm_printf("Arx kernel: cpu %u smp_kmain initialization complete\n", (unsigned) arch_cpu_id());

    for (;;)
    {
    }
}
