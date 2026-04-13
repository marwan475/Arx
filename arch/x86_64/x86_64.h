#ifndef X86_64_H
#define X86_64_H

#include <stdint.h>

typedef struct discriptor_register
{
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) discriptor_register_t;

typedef struct discriptor
{
    union
    {
        uint64_t value;
        struct
        {
            uint64_t limit_15_0 : 16;
            uint64_t base_15_0 : 16;
            uint64_t base_23_16 : 8;
            uint64_t type : 4;
            uint64_t s : 1;
            uint64_t dpl : 2;
            uint64_t p : 1;
            uint64_t limit_19_16 : 4;
            uint64_t avl : 1;
            uint64_t l : 1;
            uint64_t d_b : 1;
            uint64_t g : 1;
            uint64_t base_31_24 : 8;
        };
    };
} discriptor_t;

typedef struct tss_discriptor
{
    discriptor_t descriptor;
    uint32_t     base_high;
    uint32_t     reserved;
} tss_discriptor_t;

typedef struct tss
{
    uint32_t reserved_1;
    uint32_t RSP0_lower;
    uint32_t RSP0_upper;
    uint32_t RSP1_lower;
    uint32_t RSP1_upper;
    uint32_t RSP2_lower;
    uint32_t RSP2_upper;
    uint32_t reserved_2;
    uint32_t reserved_3;
    uint32_t IST1_lower;
    uint32_t IST1_upper;
    uint32_t IST2_lower;
    uint32_t IST2_upper;
    uint32_t IST3_lower;
    uint32_t IST3_upper;
    uint32_t IST4_lower;
    uint32_t IST4_upper;
    uint32_t IST5_lower;
    uint32_t IST5_upper;
    uint32_t IST6_lower;
    uint32_t IST6_upper;
    uint32_t IST7_lower;
    uint32_t IST7_upper;
    uint32_t reserved_4;
    uint32_t reserved_5;
    uint16_t reserved_6;
    uint16_t io_map_base;
} tss_t;

typedef struct gdt
{
    discriptor_t     null;
    discriptor_t     kernel_code_64;
    discriptor_t     kernel_data_64;
    discriptor_t     user_code_64;
    discriptor_t     user_data_64;
    tss_discriptor_t tss;
} gdt_t;

typedef struct arch_info
{
    gdt_t                 gdt;
    tss_t                 tss;
    discriptor_register_t gdt_reg;
    tss_discriptor_t      tss_descriptor;
} arch_info_t;

#endif