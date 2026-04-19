#include <klib/klib.h>

void debug_validate_boot(const struct boot_info* boot_info, uint64_t cpu_count)
{
    KDEBUG("boot summary cpu_count=%llu smp.cpu_count=%llu smp.cpus=0x%llx bsp_id=0x%llx\n", (unsigned long long) cpu_count, (unsigned long long) boot_info->smp.cpu_count, (unsigned long long) boot_info->smp.cpus, (unsigned long long) boot_info->smp.bsp_id);
    KDEBUG("memmap entries=%llu ptr=0x%llx hhdm_present=%llu hhdm_offset=0x%llx\n", (unsigned long long) boot_info->memmap_entry_count, (unsigned long long) boot_info->memmap_entries, (unsigned long long) boot_info->hhdm_present, (unsigned long long) boot_info->hhdm_offset);
    KDEBUG("rsdp=0x%llx kernel=[0x%llx..0x%llx) fb=0x%llx\n", (unsigned long long) boot_info->rsdp_address, (unsigned long long) boot_info->kernel_start, (unsigned long long) boot_info->kernel_end, (unsigned long long) boot_info->framebuffer_addr);

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
}
