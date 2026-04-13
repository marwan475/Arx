#include <arch/arch.h>
#include <klib/klib.h>

void build_tss_descriptor(const tss_t* tss, tss_discriptor_t* tss_descriptor)
{
    uint64_t tss_address = (uint64_t) tss;
    uint32_t tss_limit   = (uint32_t) (sizeof(tss_t) - 1);

    tss_descriptor->descriptor.value = 0;

    tss_descriptor->descriptor.value |= (uint64_t) (tss_limit & 0xFFFF);               
    tss_descriptor->descriptor.value |= (uint64_t) (tss_address & 0xFFFFFF) << 16;     
    tss_descriptor->descriptor.value |= (uint64_t) 0x9 << 40;                          
    tss_descriptor->descriptor.value |= (uint64_t) 1 << 47;                            
    tss_descriptor->descriptor.value |= (uint64_t) ((tss_limit >> 16) & 0xF) << 48;    
    tss_descriptor->descriptor.value |= (uint64_t) ((tss_address >> 24) & 0xFF) << 56; 

    tss_descriptor->base_high = (uint32_t) ((tss_address >> 32) & 0xFFFFFFFF);
    tss_descriptor->reserved  = 0;

}

void build_gdt(gdt_t *gdt, const tss_discriptor_t* tss_descriptor)
{

    gdt->null.value           = 0x0000000000000000;
    gdt->kernel_code_64.value = 0x00AF9A000000FFFF;
    gdt->kernel_data_64.value = 0x00CF92000000FFFF;
    gdt->user_code_64.value   = 0x00AFFA000000FFFF;
    gdt->user_data_64.value   = 0x00CFF2000000FFFF;
    gdt->tss                  = *tss_descriptor;

}

void gdt_init(){

    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    build_tss_descriptor(&cpu_info->arch_info.tss, &cpu_info->arch_info.tss_descriptor);
    build_gdt(&cpu_info->arch_info.gdt, &cpu_info->arch_info.tss_descriptor);

    cpu_info->arch_info.gdt_reg.limit = sizeof(gdt_t) - 1;
    cpu_info->arch_info.gdt_reg.base  = (uint64_t) (uintptr_t) &cpu_info->arch_info.gdt;

     __asm__ __volatile__("cli\n"
                         "lgdt %[gdt]\n"
                         "ltr %[tss]\n"

                         "pushq $0x8\n"
                         "leaq 1f(%%rip), %%rax\n"
                         "pushq %%rax\n"
                         "lretq\n"

                         "1:\n"
                
                         "movw $0x10, %%ax\n"
                         "movw %%ax, %%ds\n"
                         "movw %%ax, %%es\n"
                         "movw %%ax, %%ss\n"
                         
                         "movw %%ax, %%fs\n"
                         "movw %%ax, %%gs\n"
                         :
                         : [gdt] "m"(cpu_info->arch_info.gdt_reg), [tss] "r"((uint16_t) offsetof(gdt_t, tss))
                         : "rax", "memory");
}




