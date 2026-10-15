#include <klib/klib.h>
#include <platform.h>

#if BOOT_DIAGNOSTICS
#include <klib/printf/printf.h>
extern void arch_serial_putchar(char c);

void debug_boot_checkpoint(const char* operation)
{
    __atomic_store_n(&platform.cpus[arch_cpu_id()].boot_operation, operation, __ATOMIC_RELAXED);
    __atomic_store_n(&platform.cpus[arch_cpu_id()].boot_operation_ticks, 0, __ATOMIC_RELAXED);
}

void debug_boot_tick(const void* interrupted_pc)
{
    cpu_info_t* cpu = &platform.cpus[arch_cpu_id()];
    if (__atomic_load_n(&cpu->boot_operation, __ATOMIC_RELAXED) != NULL && __atomic_load_n(&cpu->boot_operation_ticks, __ATOMIC_RELAXED) < 1000
        && __atomic_add_fetch(&cpu->boot_operation_ticks, 1, __ATOMIC_RELAXED) == 1000)
        debug_spin_wait(NULL, interrupted_pc);
}

void debug_spin_wait(const void* lock, const void* caller)
{
    if (platform.cpus == NULL)
        return;
    if (caller == NULL)
        caller = __builtin_return_address(0);

    uint64_t flags = 0;
#if defined(__x86_64__)
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
#endif
    cpu_info_t* cpu = &platform.cpus[arch_cpu_id()];
    const char* operation = __atomic_load_n(&cpu->boot_operation, __ATOMIC_RELAXED);
    int target = -1;
    for (size_t i = 0; i < platform.cpu_count; ++i)
        if (lock == &platform.cpus[i].ipi_lock)
            target = (int) i;

    const char* target_operation = target >= 0 ? __atomic_load_n(&platform.cpus[target].boot_operation, __ATOMIC_RELAXED) : NULL;
    char line[320];
    snprintf_(line, sizeof(line), "\nBOOT WAIT cpu=%u op=%s lock=%p caller=%p flags=%llx preempt=%u ipi_target=%d pending=%u target_op=%s\n",
              (unsigned) arch_cpu_id(), operation != NULL ? operation : "bootstrap", lock, caller,
              (unsigned long long) flags, (unsigned) cpu->preemption_depth, target,
              target >= 0 ? (unsigned) __atomic_load_n(&platform.cpus[target].ipi_request_data.type, __ATOMIC_RELAXED) : 0,
              target_operation != NULL ? target_operation : "-");
    // No logger lock, allocation, clock read or blocking console acquisition.
    // Serial lines may interleave; a busy framebuffer is skipped.
    for (const char* p = line; *p; ++p)
        arch_serial_putchar(*p);
    cpu_preemption_disable();
    uint8_t expected = 0;
    if (__atomic_compare_exchange_n(&platform.terminal_lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        terminal_write(line, strlen(line));
        spinlock_release(&platform.terminal_lock);
    }
    cpu_preemption_enable();
}
#endif

void debug_validate_boot(const struct boot_info* boot_info, uint64_t cpu_count)
{
    KDEBUG("boot summary cpu_count=%llu smp.cpu_count=%llu smp.cpus=0x%llx bsp_id=0x%llx\n", (unsigned long long) cpu_count, (unsigned long long) boot_info->smp.cpu_count, (unsigned long long) boot_info->smp.cpus, (unsigned long long) boot_info->smp.bsp_id);
    KDEBUG("memmap entries=%llu ptr=0x%llx hhdm_present=%llu hhdm_offset=0x%llx\n", (unsigned long long) boot_info->memmap_entry_count, (unsigned long long) boot_info->memmap_entries, (unsigned long long) boot_info->hhdm_present, (unsigned long long) boot_info->hhdm_offset);
    KDEBUG("rsdp=0x%llx kernel=[0x%llx..0x%llx) fb=0x%llx\n", (unsigned long long) boot_info->rsdp_address, (unsigned long long) boot_info->kernel_start, (unsigned long long) boot_info->kernel_end, (unsigned long long) boot_info->framebuffer_addr);
    KDEBUG("initramfs present=%llu addr=0x%llx size=%llu path=%s cmdline=%s\n", (unsigned long long) boot_info->initramfs_present, (unsigned long long) boot_info->initramfs_address, (unsigned long long) boot_info->initramfs_size,
           boot_info->initramfs_path != 0 ? (const char*) (uintptr_t) boot_info->initramfs_path : "<none>", boot_info->initramfs_cmdline != 0 ? (const char*) (uintptr_t) boot_info->initramfs_cmdline : "<none>");

    if (cpu_count > BOOT_SMP_MAX_CPUS)
    {
        KDEBUG("warning cpu_count=%llu exceeds BOOT_SMP_MAX_CPUS=%u, clamping\n", (unsigned long long) cpu_count, BOOT_SMP_MAX_CPUS);
    }

    if (boot_info->smp.cpu_count > BOOT_SMP_MAX_CPUS)
    {
        KDEBUG("warning smp.cpu_count=%llu exceeds BOOT_SMP_MAX_CPUS=%u\n", (unsigned long long) boot_info->smp.cpu_count, BOOT_SMP_MAX_CPUS);
    }

    if (boot_info->hhdm_present == 0)
    {
        KDEBUG("warning HHDM missing (required by PMM/VMM paths)\n");
    }

    if (boot_info->rsdp_address == 0)
    {
        KDEBUG("warning RSDP missing, ACPI init likely to degrade\n");
    }

    if (boot_info->framebuffer_bpp != 32 && boot_info->framebuffer_bpp != 24)
    {
        KDEBUG("warning unusual framebuffer bpp=%llu\n", (unsigned long long) boot_info->framebuffer_bpp);
    }

    if (boot_info->framebuffer_width > 0 && boot_info->framebuffer_bpp > 0)
    {
        uint64_t min_pitch = boot_info->framebuffer_width * (boot_info->framebuffer_bpp / 8);
        if (boot_info->framebuffer_pitch < min_pitch)
        {
            KDEBUG("warning framebuffer pitch=%llu < min expected=%llu\n", (unsigned long long) boot_info->framebuffer_pitch, (unsigned long long) min_pitch);
        }
    }

    if (boot_info->initramfs_present == 0)
    {
        KDEBUG("warning initramfs module missing\n");
    }
    else
    {
        if (boot_info->initramfs_address == 0)
        {
            KDEBUG("warning initramfs address is zero\n");
        }

        if (boot_info->initramfs_size == 0)
        {
            KDEBUG("warning initramfs size is zero\n");
        }

        if (boot_info->initramfs_path == 0)
        {
            KDEBUG("warning initramfs path missing\n");
        }
    }
}

void debug_pci_devices(void)
{
    KDEBUG("PCI devices count=%llu ptr=0x%llx\n", (unsigned long long) platform.pci_device_count, (unsigned long long) (uintptr_t) platform.pci_devices);

    if (platform.pci_devices == NULL || platform.pci_device_count == 0)
    {
        KDEBUG("PCI devices: none\n");
        return;
    }

    for (size_t i = 0; i < platform.pci_device_count; i++)
    {
        const pci_device_t* dev = &platform.pci_devices[i];

        KDEBUG("PCI[%llu] seg=%u %02x:%02x.%u vendor=%04x device=%04x class=%02x:%02x:%02x hdr=%02x cmd=%04x sts=%04x\n", (unsigned long long) i, (unsigned) dev->segment, (unsigned) dev->bus, (unsigned) dev->device, (unsigned) dev->function, (unsigned) dev->vendor_id, (unsigned) dev->device_id,
               (unsigned) dev->class_code, (unsigned) dev->subclass, (unsigned) dev->prog_if, (unsigned) dev->header_type, (unsigned) dev->command, (unsigned) dev->status);
    }
}
