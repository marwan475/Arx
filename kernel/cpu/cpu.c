#include <cpu/cpu.h>
#include <klib/klib.h>

void cpus_init(size_t cpu_count)
{
    for (size_t i = 0; i < cpu_count; i++)
    {
        dispatcher.cpus[i].id            = i;
        dispatcher.cpus[i].numa_node     = NULL;
        dispatcher.cpus[i].address_space = NULL;
    }

    dispatcher.cpu_count = cpu_count;
}
