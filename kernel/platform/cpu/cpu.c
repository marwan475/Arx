#include <cpu/cpu.h>
#include <klib/klib.h>
#include <platform.h>
#include <platform/memory/vmm.h>

extern void dispatcher_service_pending_reschedule(void);

platform_t platform;

static cpu_info_t platform_cpus[BOOT_SMP_MAX_CPUS];

void cpus_init(size_t cpu_count)
{
    size_t bounded_cpu_count = cpu_count > BOOT_SMP_MAX_CPUS ? BOOT_SMP_MAX_CPUS : cpu_count;

    platform.cpus = platform_cpus;
    memset(&platform.arch_info, 0, sizeof(platform.arch_info));
    memset(platform.cpus, 0, sizeof(platform_cpus));

    for (size_t i = 0; i < bounded_cpu_count; i++)
    {
        platform.cpus[i].id = i;
    }

    platform.cpu_count = bounded_cpu_count;
    arch_init_cpu_info();
}

__attribute__((noreturn)) void cpu_init_stack(arch_stack_entry_t entry, void* arg)
{
    cpu_info_t* cpu = &platform.cpus[arch_cpu_id()];

    if (cpu->kernel_stack_base == NULL)
    {
        void* region = vmalloc(CPU_KERNEL_STACK_SIZE + PAGE_SIZE);

        if (region == NULL)
        {
            error_logger("Arx kernel: failed to allocate kernel stack for CPU %d\n", arch_cpu_id());
            panic();
        }

        vmm_unmap_page((uintptr_t) region, vmm_kernel_space());
        cpu->kernel_stack_base = (uint8_t*) region + PAGE_SIZE;
        cpu->kernel_stack_size = CPU_KERNEL_STACK_SIZE;
    }

    void* stack_top = (void*) ((uint8_t*) cpu->kernel_stack_base + cpu->kernel_stack_size);
    arch_set_stack(stack_top, entry, arg);
}

void cpu_preemption_disable(void)
{
    if (platform.cpus != NULL)
    {
        __atomic_add_fetch(&platform.cpus[arch_cpu_id()].preemption_depth, 1, __ATOMIC_RELAXED);
    }
}

void cpu_preemption_enable(void)
{
    if (platform.cpus != NULL)
    {
        cpu_info_t* cpu = &platform.cpus[arch_cpu_id()];

        if (__atomic_load_n(&cpu->preemption_depth, __ATOMIC_RELAXED) != 0)
        {
            const uint32_t remaining = __atomic_sub_fetch(&cpu->preemption_depth, 1, __ATOMIC_RELAXED);
            if (remaining == 0 && arch_task_context_supported() && arch_interrupts_enabled() && !cpu->switching_context && __atomic_load_n(&cpu->reschedule_pending, __ATOMIC_ACQUIRE))
            {
                dispatcher_service_pending_reschedule();
            }
        }
    }
}
