#include <acpi/acpi.h>
#include <boot/boot.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <stdint.h>
#include <terminal/terminal.h>

void run_selftests(void);
void kmain_post_init(void* arg);

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

    // Can access per cpu structs from dispatcher after this function
    KDEBUG("-> cpus_init(%llu)\n", (unsigned long long) cpu_count);
    cpus_init(cpu_count);
    KDEBUG("<- cpus_init done dispatcher.cpu_count=%llu\n", (unsigned long long) dispatcher.cpu_count);

    KDEBUG("-> pmm_init\n");
    pmm_init(boot_info);
    KDEBUG("<- pmm_init done\n");

    KDEBUG("-> vmm_init\n");
    vmm_init(boot_info);
    KDEBUG("<- vmm_init done\n");

    KDEBUG("-> heap_init\n");
    heap_init();
    KDEBUG("<- heap_init done\n");

    KDEBUG("-> acpi_init(rsdp=0x%llx)\n", (unsigned long long) boot_info->rsdp_address);
    acpi_init(boot_info->rsdp_address);
    KDEBUG("<- acpi_init done\n");

    KDEBUG("-> arch_init\n");
    status = arch_init();
    KDEBUG("<- arch_init done\n");

    if (!status)
    {
        kprintf("Arx kernel: architecture initialization failed\n");
        kterm_printf("Arx kernel: architecture initialization failed\n");
        panic();
    }

    KDEBUG("-> enumerate_devices\n");
    enumerate_devices();
    KDEBUG("<- enumerate_devices done\n");

    dispatcher.cpus[arch_cpu_id()].initialized = true;

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

    KDEBUG("-> run_selftests\n");
    run_selftests();
    KDEBUG("<- run_selftests done\n");

    kterm_printf("Arx kernel: initialization complete\n");

    cpu_init_stack(kmain_post_init, 0);
}

void smp_kmain(void)
{
    kprintf("Arx kernel: cpu %d entered smp_kmain\n", arch_cpu_id());

    arch_init();

    kterm_printf("Arx kernel: cpu %u smp_kmain initialization complete\n", (unsigned) arch_cpu_id());

    dispatcher.cpus[arch_cpu_id()].initialized = true;

    cpu_init_stack(kmain_post_init, 0);
}

void kmain_post_init(void* arg)
{

    dispatcher.cpus_initialized++;
    while(dispatcher.cpus_initialized < dispatcher.cpu_count)
    {
        arch_pause();
    }

    (void) arg;
    kprintf("Arx kernel: cpu %d kmain_post_init entered\n", arch_cpu_id());
    KDEBUG("cpu %d kmain_post_init entered\n", arch_cpu_id());
    
    for (;;)
    {
    }
}

