#include <acpi/acpi.h>
#include <boot/boot.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <platform.h>
#include <selftests/selftests.h>
#include <stdint.h>
#include <terminal/terminal.h>

void platform_init_complete(void* arg);
void kmain(void);

// From bootloader we need
// - memory map
// - framebuffer info
// - smp info
// - all cores having same address space/pagetable
// - higher half direct map instead of identity mapping so user address space is separate from physical memory addresses
// - paging with no user access and RWX on direct map
// - acpi rsdp address
void platform_init(struct boot_info* boot_info, uint64_t cpu_count)
{
    // Start platform initialization

    bool status = true;

    ARX_DEBUG_STORE(platform.debugStats.stage, ARX_PLATFORM_BOOT_ENTERED);

    kprintf("Arx kernel: platform_init entered\n");

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

    if (boot_info->initramfs_present == 0)
    {
        kprintf("Arx kernel: no initramfs module\n");
        panic();
    }

    if (boot_info->initramfs_address == 0 || boot_info->initramfs_size == 0)
    {
        kprintf("Arx kernel: invalid initramfs module (addr=0x%llx size=%llu)\n", (unsigned long long) boot_info->initramfs_address, (unsigned long long) boot_info->initramfs_size);
        panic();
    }

    if (boot_info->initramfs_path == 0)
    {
        kprintf("Arx kernel: initramfs module path missing\n");
        panic();
    }

    kprintf("Arx kernel: initramfs module loaded path=%s addr=0x%llx size=%llu\n", (const char*) (uintptr_t) boot_info->initramfs_path, (unsigned long long) boot_info->initramfs_address, (unsigned long long) boot_info->initramfs_size);

    platform.initramfs_address = (uintptr_t) boot_info->initramfs_address;
    platform.initramfs_size    = boot_info->initramfs_size;

    platform.framebuffer.address          = (void*) (uintptr_t) boot_info->framebuffer_addr;
    platform.framebuffer.width            = (size_t) boot_info->framebuffer_width;
    platform.framebuffer.height           = (size_t) boot_info->framebuffer_height;
    platform.framebuffer.pitch            = (size_t) boot_info->framebuffer_pitch;
    platform.framebuffer.red_mask_size    = (uint8_t) boot_info->framebuffer_red_mask_size;
    platform.framebuffer.red_mask_shift   = (uint8_t) boot_info->framebuffer_red_mask_shift;
    platform.framebuffer.green_mask_size  = (uint8_t) boot_info->framebuffer_green_mask_size;
    platform.framebuffer.green_mask_shift = (uint8_t) boot_info->framebuffer_green_mask_shift;
    platform.framebuffer.blue_mask_size   = (uint8_t) boot_info->framebuffer_blue_mask_size;
    platform.framebuffer.blue_mask_shift  = (uint8_t) boot_info->framebuffer_blue_mask_shift;

    if (!terminal_init(&platform.framebuffer))
    {
        error_logger("Arx kernel: failed to initialize terminal\n");
        panic();
    }

    kterm_printf("Arx kernel: terminal initialized\n");

    debug_validate_boot(boot_info, cpu_count);

    // Can access per cpu structs from platform after this function
    KDEBUG("-> cpus_init(%llu)\n", (unsigned long long) cpu_count);
    cpus_init(cpu_count);
    KDEBUG("<- cpus_init done platform.cpu_count=%llu\n", (unsigned long long) platform.cpu_count);

    if (boot_info->smp.cpu_count > 0 && boot_info->smp.cpus != 0)
    {
        platform.bsp_id = 0;
        arch_boot_cpu_hardware_ids(boot_info);
    }
    else
    {
        platform.bsp_id = (uint64_t) arch_cpu_id();
    }

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
    ARX_DEBUG_STORE(platform.debugStats.stage, ARX_PLATFORM_ARCH_INITIALIZING);
    ARX_DEBUG_STORE(platform.debugStats.cpus.stages[arch_cpu_id()], ARX_CPU_ARCH_INITIALIZING);
    status = arch_init();
    KDEBUG("<- arch_init done\n");

    if (!status)
    {
        error_logger("Arx kernel: architecture initialization failed\n");
        kterm_printf("Arx kernel: architecture initialization failed\n");
        panic();
    }

    arch_init_platform_dma();

    __atomic_store_n(&platform.cpus[arch_cpu_id()].initialized, true, __ATOMIC_RELEASE);
    ARX_DEBUG_STORE(platform.debugStats.cpus.stages[arch_cpu_id()], ARX_CPU_ARCH_INITIALIZED);
    ARX_DEBUG_ADD(platform.debugStats.cpus.architectureInitialized, 1);

    arch_smp_init(boot_info);

    ARX_DEBUG_STORE(platform.debugStats.stage, ARX_PLATFORM_WAITING_FOR_CPUS);

    bool waiting_for_other_cpus = true;

    while (waiting_for_other_cpus)
    {
        for (size_t i = 0; i < platform.cpu_count; i++)
        {
            if (!__atomic_load_n(&platform.cpus[i].initialized, __ATOMIC_ACQUIRE))
            {
                waiting_for_other_cpus = true;
                break;
            }

            waiting_for_other_cpus = false;
        }
    }

    KDEBUG("-> platform_selftests\n");
    ARX_DEBUG_STORE(platform.debugStats.stage, ARX_PLATFORM_SELFTESTS);
    platform_selftests();
    KDEBUG("<- platform_selftests done\n");

    kterm_printf("Arx kernel: kernel bootstrap done\n");

    ARX_DEBUG_STORE(platform.debugStats.stage, ARX_PLATFORM_STACK_BARRIER);
    cpu_init_stack(platform_init_complete, 0);
}

void smp_platform_init(void)
{
    kprintf("Arx kernel: cpu %d entered smp_platform_init\n", arch_cpu_id());

    ARX_DEBUG_STORE(platform.debugStats.cpus.stages[arch_cpu_id()], ARX_CPU_ARCH_INITIALIZING);
    arch_init();

    kterm_printf("Arx kernel: cpu %u smp_platform_init initialization complete\n", (unsigned) arch_cpu_id());

    __atomic_store_n(&platform.cpus[arch_cpu_id()].initialized, true, __ATOMIC_RELEASE);
    ARX_DEBUG_STORE(platform.debugStats.cpus.stages[arch_cpu_id()], ARX_CPU_ARCH_INITIALIZED);
    ARX_DEBUG_ADD(platform.debugStats.cpus.architectureInitialized, 1);

    cpu_init_stack(platform_init_complete, 0);
}

void platform_init_complete(void* arg)
{
    // Platform initialization complete

    ARX_DEBUG_STORE(platform.debugStats.cpus.stages[arch_cpu_id()], ARX_CPU_STACK_ARRIVED);
    ARX_DEBUG_ADD(platform.debugStats.cpus.stackArrivals, 1);
    __atomic_fetch_add(&platform.cpus_initialized, 1, __ATOMIC_ACQ_REL);

    while (__atomic_load_n(&platform.cpus_initialized, __ATOMIC_ACQUIRE) < platform.cpu_count)
    {
        arch_pause();
    }

    kprintf("Arx kernel: cpu %d platform_init_complete entered\n", arch_cpu_id());
    KDEBUG("cpu %d platform_init_complete entered\n", arch_cpu_id());

    (void) arg;

    if ((uint64_t) arch_cpu_id() == platform.bsp_id)
    {
        kmain();
    }
    else
    {
        while (__atomic_load_n(&platform.bsp_kmain_exited, __ATOMIC_ACQUIRE) == 0)
        {
            arch_pause();
        }

        kprintf("Arx kernel: cpu %d awaiting scheduler dispatch\n", arch_cpu_id());

        for (;;)
        {
            arch_pause();
        }
    }

    for (;;)
    {
        arch_pause();
    }
}
