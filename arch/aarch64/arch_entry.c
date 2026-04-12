/*
 * arch_entry.c
 *
 * Entry point for architecture-specific initialization for pre kernel handoff
 *
 * - AArch64 pre-kernel entry point and SMP entry point
 *
 * Author: Marwan Mostafa
 *
 */

#include <boot/boot.h>
#include <boot/limine.h>
#include <klib/klib.h>
#include <stdint.h>

__attribute__((used, section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".limine_requests"))) static volatile struct limine_memmap_request memmap_request = {
        .id       = LIMINE_MEMMAP_REQUEST,
        .revision = 0,
        .response = 0,
};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_framebuffer_request framebuffer_request = {
        .id       = LIMINE_FRAMEBUFFER_REQUEST,
        .revision = 0,
        .response = 0,
};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_hhdm_request hhdm_request = {
        .id       = LIMINE_HHDM_REQUEST,
        .revision = 0,
        .response = 0,
};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_rsdp_request rsdp_request = {
    .id       = LIMINE_RSDP_REQUEST,
    .revision = 0,
    .response = 0,
};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_paging_mode_request paging_mode_request = {
        .id       = LIMINE_PAGING_MODE_REQUEST,
        .revision = 0,
        .response = 0,
        .mode     = LIMINE_PAGING_MODE_DEFAULT,
        .max_mode = LIMINE_PAGING_MODE_DEFAULT,
        .min_mode = LIMINE_PAGING_MODE_MIN,
};

__attribute__((used, section(".limine_requests"))) static volatile struct limine_smp_request smp_request = {
        .id       = LIMINE_SMP_REQUEST,
        .revision = 0,
        .response = 0,
        .flags    = 0,
};

__attribute__((used, section(".limine_requests_start"))) static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;

static struct boot_memmap_entry boot_memmap[BOOT_MEMMAP_MAX_ENTRIES];

static uintptr_t pl011_base = 0x09000000u;

static uint64_t read_sctlr_el1(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return sctlr;
}

static void write_sctlr_el1(uint64_t sctlr)
{
    __asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
    __asm__ volatile("isb");
}

static void ensure_paging_enabled(void)
{
    uint64_t sctlr = read_sctlr_el1();

    if ((sctlr & (1ull << 0)) == 0)
    {
        sctlr |= (1ull << 0);
        write_sctlr_el1(sctlr);
    }
}

static inline volatile unsigned int* pl011_reg(unsigned int offset)
{
    return (volatile unsigned int*) (pl011_base + offset);
}

void arch_halt(void)
{
    __asm__ volatile("wfi");
}

void arch_pause(void)
{
    __asm__ volatile("yield");
}

static inline void arch_enable_fp_simd(void)
{
    uint64_t cpacr;

    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3ull << 20);
    __asm__ volatile("msr cpacr_el1, %0" : : "r"(cpacr));
    __asm__ volatile("isb");
}

static void arch_serial_init(void)
{
    static int initialized = 0;

    if (initialized)
    {
        return;
    }

    if (hhdm_request.response != 0)
    {
        pl011_base = (uintptr_t) (hhdm_request.response->offset + 0x09000000u);
    }

    *pl011_reg(0x30) = 0;
    *pl011_reg(0x44) = 0x7ffu;

    *pl011_reg(0x24) = 13;
    *pl011_reg(0x28) = 1;

    *pl011_reg(0x2c) = (1u << 4) | (3u << 5);
    *pl011_reg(0x30) = (1u << 0) | (1u << 8) | (1u << 9);

    initialized = 1;
}

void arch_serial_putchar(char c)
{
    for (unsigned int i = 0; i < 1000000; i++)
    {
        if (((*pl011_reg(0x18)) & (1u << 5)) == 0)
        {
            break;
        }
    }

    *pl011_reg(0x00) = (unsigned int) c;
}

static void serial_write_string(const char* s)
{
    while (*s != '\0')
    {
        if (*s == '\n')
        {
            arch_serial_putchar('\r');
        }

        arch_serial_putchar(*s);
        s++;
    }
}

static void smp_entry(struct limine_smp_info* cpu)
{
    const char* arg = (const char*) (uintptr_t) cpu->extra_argument;

    arch_enable_fp_simd();
    if (arg != 0)
    {
        kprintf("Arx kernel: cpu[%u] boot entry: %s\n", cpu->processor_id, arg);
    }
    else
    {
        kprintf("Arx kernel: cpu[%u] boot entry\n", cpu->processor_id);
    }

    for (;;)
    {
        __asm__ volatile("wfi");
    }
}

static void start_core(struct limine_smp_info* cpu)
{
    volatile struct limine_smp_info* vcpu               = (volatile struct limine_smp_info*) cpu;
    static const char                cpu_boot_message[] = "hello from cpu entry\n";

    vcpu->extra_argument = (uint64_t) (uintptr_t) cpu_boot_message;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    vcpu->goto_address = smp_entry;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __asm__ volatile("dsb ishst" : : : "memory");
    __asm__ volatile("sev; sev; sev" : : : "memory");
}

void arch_smp_init(struct boot_info* boot_info)
{
    struct limine_smp_info** smp_cpus;

    if (boot_info->smp.cpu_count == 0 || boot_info->smp.cpus == 0)
    {
        kprintf("Arx kernel: smp cpu array unavailable\n");
        return;
    }

    smp_cpus = (struct limine_smp_info**) (uintptr_t) boot_info->smp.cpus;
    for (uint64_t i = 0; i < boot_info->smp.cpu_count; i++)
    {
        uint64_t cpu_hw_id = smp_cpus[i]->mpidr;

        if (cpu_hw_id != boot_info->smp.bsp_id)
        {
            start_core(smp_cpus[i]);
        }
    }
}

static void gather_boot_info(struct boot_info* boot_info)
{
    boot_info->limine_present     = 0;
    boot_info->rsdp_address       = 0;
    boot_info->hhdm_present       = 0;
    boot_info->hhdm_offset        = 0;
    boot_info->memmap_entry_count = 0;
    boot_info->memmap_entries     = 0;
    boot_info->framebuffer_addr   = 0;
    boot_info->framebuffer_width  = 0;
    boot_info->framebuffer_height = 0;
    boot_info->framebuffer_pitch  = 0;
    boot_info->framebuffer_bpp    = 0;
    boot_info->smp.flags          = 0;
    boot_info->smp.bsp_id         = 0;
    boot_info->smp.cpu_count      = 0;
    boot_info->smp.cpus           = 0;

    if (!LIMINE_BASE_REVISION_SUPPORTED)
    {
        return;
    }

    boot_info->limine_present = 1;

    if (hhdm_request.response != 0)
    {
        boot_info->hhdm_present = 1;
        boot_info->hhdm_offset  = hhdm_request.response->offset;
    }

    if (rsdp_request.response != 0)
    {
        boot_info->rsdp_address = (uint64_t) (uintptr_t) rsdp_request.response->address;
    }

    if (memmap_request.response != 0)
    {
        uint64_t count = memmap_request.response->entry_count;
        if (count > BOOT_MEMMAP_MAX_ENTRIES)
        {
            count = BOOT_MEMMAP_MAX_ENTRIES;
        }

        for (uint64_t i = 0; i < count; i++)
        {
            struct limine_memmap_entry* entry = memmap_request.response->entries[i];
            boot_memmap[i].base               = entry->base;
            boot_memmap[i].length             = entry->length;
            boot_memmap[i].type               = entry->type;
        }

        boot_info->memmap_entry_count = count;
        boot_info->memmap_entries     = (uintptr_t) boot_memmap;
    }

    if (framebuffer_request.response != 0 && framebuffer_request.response->framebuffer_count > 0)
    {
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];

        boot_info->framebuffer_addr   = (uint64_t) (uintptr_t) fb->address;
        boot_info->framebuffer_width  = fb->width;
        boot_info->framebuffer_height = fb->height;
        boot_info->framebuffer_pitch  = fb->pitch;
        boot_info->framebuffer_bpp    = fb->bpp;
    }

    if (smp_request.response != 0)
    {
        uint64_t count = smp_request.response->cpu_count;
        if (count > BOOT_SMP_MAX_CPUS)
        {
            count = BOOT_SMP_MAX_CPUS;
        }

        boot_info->smp.flags     = smp_request.response->flags;
        boot_info->smp.bsp_id    = smp_request.response->bsp_mpidr;
        boot_info->smp.cpu_count = count;
        boot_info->smp.cpus      = (uintptr_t) smp_request.response->cpus;
    }
}

void _start(void)
{
    struct boot_info boot_info;
    uint64_t         cpu_count = 1;

    ensure_paging_enabled();
    arch_enable_fp_simd();
    arch_serial_init();
    serial_write_string("Arx kernel: aarch64 entry\n");
    gather_boot_info(&boot_info);

    if (boot_info.smp.cpu_count > 0)
    {
        cpu_count = boot_info.smp.cpu_count;
    }

    kmain(&boot_info, cpu_count);

    for (;;)
    {
        __asm__ volatile("wfi");
    }
}
