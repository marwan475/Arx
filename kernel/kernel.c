#include <acpi/acpi.h>
#include <boot/boot.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <terminal/terminal.h>
#include <stdint.h>

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

    kernel_framebuffer_t framebuffer = {
        .address          = (void*) (uintptr_t) boot_info->framebuffer_addr,
        .width            = (size_t) boot_info->framebuffer_width,
        .height           = (size_t) boot_info->framebuffer_height,
        .pitch            = (size_t) boot_info->framebuffer_pitch,
        .red_mask_size    = (uint8_t) boot_info->framebuffer_red_mask_size,
        .red_mask_shift   = (uint8_t) boot_info->framebuffer_red_mask_shift,
        .green_mask_size  = (uint8_t) boot_info->framebuffer_green_mask_size,
        .green_mask_shift = (uint8_t) boot_info->framebuffer_green_mask_shift,
        .blue_mask_size   = (uint8_t) boot_info->framebuffer_blue_mask_size,
        .blue_mask_shift  = (uint8_t) boot_info->framebuffer_blue_mask_shift,
    };

    if (!terminal_init(&framebuffer))
    {
        kprintf("Arx kernel: failed to initialize terminal\n");
        panic();
    }
    
    kterm_printf("Arx kernel: terminal initialized\n");

    KDEBUG("Arx debug: boot summary cpu_count=%llu smp.cpu_count=%llu smp.cpus=0x%llx bsp_id=0x%llx\n", (unsigned long long) cpu_count, (unsigned long long) boot_info->smp.cpu_count,
           (unsigned long long) boot_info->smp.cpus, (unsigned long long) boot_info->smp.bsp_id);
    KDEBUG("Arx debug: memmap entries=%llu ptr=0x%llx hhdm_present=%llu hhdm_offset=0x%llx\n", (unsigned long long) boot_info->memmap_entry_count,
           (unsigned long long) boot_info->memmap_entries, (unsigned long long) boot_info->hhdm_present, (unsigned long long) boot_info->hhdm_offset);
    KDEBUG("Arx debug: rsdp=0x%llx kernel=[0x%llx..0x%llx) fb=0x%llx\n", (unsigned long long) boot_info->rsdp_address, (unsigned long long) boot_info->kernel_start,
           (unsigned long long) boot_info->kernel_end, (unsigned long long) boot_info->framebuffer_addr);

    if (cpu_count > BOOT_SMP_MAX_CPUS)
    {
        KDEBUG("Arx debug: warning cpu_count=%llu exceeds BOOT_SMP_MAX_CPUS=%u, clamping\n", (unsigned long long) cpu_count, BOOT_SMP_MAX_CPUS);
    }

    if (boot_info->smp.cpu_count > BOOT_SMP_MAX_CPUS)
    {
        KDEBUG("Arx debug: warning smp.cpu_count=%llu exceeds BOOT_SMP_MAX_CPUS=%u\n", (unsigned long long) boot_info->smp.cpu_count, BOOT_SMP_MAX_CPUS);
    }

    if (boot_info->hhdm_present == 0)
    {
        KDEBUG("Arx debug: warning HHDM missing (required by PMM/VMM paths)\n");
    }

    if (boot_info->rsdp_address == 0)
    {
        KDEBUG("Arx debug: warning RSDP missing, ACPI init likely to degrade\n");
    }

    if (boot_info->framebuffer_bpp != 32 && boot_info->framebuffer_bpp != 24)
    {
        KDEBUG("Arx debug: warning unusual framebuffer bpp=%llu\n", (unsigned long long) boot_info->framebuffer_bpp);
    }

    if (boot_info->framebuffer_width > 0 && boot_info->framebuffer_bpp > 0)
    {
        uint64_t min_pitch = boot_info->framebuffer_width * (boot_info->framebuffer_bpp / 8);
        if (boot_info->framebuffer_pitch < min_pitch)
        {
            KDEBUG("Arx debug: warning framebuffer pitch=%llu < min expected=%llu\n", (unsigned long long) boot_info->framebuffer_pitch, (unsigned long long) min_pitch);
        }
    }

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
    arch_init();
    KDEBUG("Arx debug: <- arch_init done\n");

    KDEBUG("Arx debug: -> run_selftests\n");
    run_selftests();
    KDEBUG("Arx debug: <- run_selftests done\n");

    kterm_printf("Arx kernel: initialization complete\n");

    arch_smp_init(boot_info);

    for (;;)
    {
    }
}

void smp_kmain(void)
{
    kprintf("Arx kernel: cpu %d entered smp_kmain\n", arch_cpu_id());

    kterm_printf("Arx kernel: cpu %u smp_kmain initialization complete\n", (unsigned) arch_cpu_id());

    arch_init();

    for (;;)
    {
    }
}
