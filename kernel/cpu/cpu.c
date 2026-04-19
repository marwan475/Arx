#include <cpu/cpu.h>
#include <klib/klib.h>

void cpus_init(size_t cpu_count)
{
    size_t bounded_cpu_count = cpu_count > BOOT_SMP_MAX_CPUS ? BOOT_SMP_MAX_CPUS : cpu_count;

    memset(&dispatcher.arch_info, 0, sizeof(dispatcher.arch_info));
    memset(dispatcher.cpus, 0, sizeof(dispatcher.cpus));

#if defined(__x86_64__)
    dispatcher.arch = ARCH_X86_64;
#elif defined(__aarch64__)
    dispatcher.arch = ARCH_AARCH64;
#else
#error Unsupported architecture
#endif

    for (size_t i = 0; i < bounded_cpu_count; i++)
    {
        dispatcher.cpus[i].id = i;
    }

    dispatcher.cpu_count = bounded_cpu_count;
}

__attribute__((noreturn)) void cpu_init_stack(arch_stack_entry_t entry, void* arg)
{
    cpu_info_t* cpu = &dispatcher.cpus[arch_cpu_id()];

    if (cpu->kernel_stack_base == NULL)
    {
        void* stack = vmalloc(CPU_KERNEL_STACK_SIZE);
        if (stack == NULL)
        {
            kprintf("Arx kernel: failed to allocate kernel stack for CPU %d\n", arch_cpu_id());
            panic();
        }

        cpu->kernel_stack_base = stack;
        cpu->kernel_stack_size = CPU_KERNEL_STACK_SIZE;
    }

    void* stack_top = (void*) ((uint8_t*) cpu->kernel_stack_base + cpu->kernel_stack_size);
    arch_set_stack(stack_top, entry, arg);
}