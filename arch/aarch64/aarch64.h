#ifndef AARCH64_H
#define AARCH64_H

#include <stdint.h>

// Layout matches the boot protocol SMP record.
struct boot_smp_cpu_info
{
    uint32_t  processor_id;
    uint32_t  reserved1;
    uint64_t  mpidr;
    uint64_t  reserved;
    uintptr_t goto_address;
    uint64_t  extra_argument;
};

struct arch_task_context
{
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;
    uint64_t sp;
    uint64_t pc;
};

typedef struct arch_syscall_frame
{
    uint64_t syscall_number;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
    uint64_t arg5;
    uint64_t user_elr;
    uint64_t user_spsr;
    uint64_t user_sp;
} arch_syscall_frame_t;

typedef struct arch_info
{
} arch_info_t;

typedef struct arch_platform_info
{
} arch_platform_info_t;

#endif