#include <arch/arch.h>

bool arch_dma_coherent_memory_supported(void)
{
    // HHDM allocations use ordinary cached RAM; require the boot WB PAT slot.
    return (x86_read_msr(0x277) & 0xFF) == 6;
}

void arch_dma_write_barrier(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

void arch_dma_read_barrier(void)
{
    __asm__ volatile("lfence" ::: "memory");
}
