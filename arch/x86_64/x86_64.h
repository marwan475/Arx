#ifndef X86_64_H
#define X86_64_H

#include <stdint.h>

#define NUM_IDT_ENTRIES 256

#define GDT_CODE_SEGMENT 0x08
#define GDT_DATA_SEGMENT 0x10
#define GDT_USER_DATA_SEGMENT 0x20
#define GDT_USER_CODE_SEGMENT 0x18

#define KERNEL_CS GDT_CODE_SEGMENT
#define KERNEL_SS GDT_DATA_SEGMENT
#define USER_CS (GDT_USER_CODE_SEGMENT | 0x3)
#define USER_SS (GDT_USER_DATA_SEGMENT | 0x3)
#define IDT_DEFAULT_GATE_FLAGS (IDT_FLAG_PRESENT | IDT_FLAG_RING0 | IDT_FLAG_GATE_32BIT_INT)

#define PIC_MASTER_DATA_PORT 0x21
#define PIC_SLAVE_DATA_PORT 0xA1

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_ENABLE (1ULL << 11)

#define LAPIC_REG_TPR 0x80
#define LAPIC_REG_EOI 0xB0
#define LAPIC_REG_SVR 0xF0
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_LVT_LINT1 0x360
#define LAPIC_REG_LVT_ERROR 0x370
#define LAPIC_REG_INITIAL_COUNT 0x380
#define LAPIC_REG_DIVIDE_CONFIG 0x3E0

#define LAPIC_SVR_SW_ENABLE (1U << 8)
#define LAPIC_LVT_MASKED (1U << 16)
#define LAPIC_LVT_TIMER_PERIODIC (1U << 17)
#define LAPIC_TIMER_VECTOR 0x20
#define LAPIC_TIMER_DIVIDE_BY_16 0x3
#define LAPIC_TIMER_INITIAL_COUNT 10000000U

#define IOAPIC_REG_IOREGSEL 0x00
#define IOAPIC_REG_IOWIN 0x10

#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VER 0x01

#define IOAPIC_REDIR_TABLE_BASE 0x10
#define IOAPIC_REDIR_MASKED (1U << 16)
#define IOAPIC_REDIR_POLARITY_LOW (1U << 13)
#define IOAPIC_REDIR_TRIGGER_LEVEL (1U << 15)

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

typedef struct idt_entry
{
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct idt_description
{
    uint16_t     limit;
    idt_entry_t* ptr;
} __attribute__((packed)) idt_description_t;

typedef enum IDT_FLAGS
{
    IDT_FLAG_GATE_TASK       = 0x5,
    IDT_FLAG_GATE_16BIT_INT  = 0x6,
    IDT_FLAG_GATE_16BIT_TRAP = 0x7,
    IDT_FLAG_GATE_32BIT_INT  = 0xE,
    IDT_FLAG_GATE_32BIT_TRAP = 0xF,

    IDT_FLAG_RING0 = (0 << 5),
    IDT_FLAG_RING1 = (1 << 5),
    IDT_FLAG_RING2 = (2 << 5),
    IDT_FLAG_RING3 = (3 << 5),

    IDT_FLAG_PRESENT = 0x80,
} IDT_FLAGS_T;

typedef struct registers
{
    uint64_t rax;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;

    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    uint64_t interrupt_number;
    uint64_t error_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;

} __attribute__((packed)) registers_t;

typedef struct arch_info
{
    uint8_t               acpi_has_lapic;
    uint64_t              acpi_lapic_base_addr;
    uint32_t              acpi_processor_uid;
    uint32_t              acpi_lapic_id;
    uint32_t              acpi_lapic_flags;
    gdt_t                 gdt;
    tss_t                 tss;
    discriptor_register_t gdt_reg;
    tss_discriptor_t      tss_descriptor;
    idt_description_t     idt_desc;
    idt_entry_t           idt[NUM_IDT_ENTRIES];
} arch_info_t;

#define LEGACY_PIC_MAX_IRQS 16

#define IRQ_VECTOR_BASE (LAPIC_TIMER_VECTOR + 1)
#define VECTOR_DEVICE_BASE (IRQ_VECTOR_BASE + LEGACY_PIC_MAX_IRQS)

typedef struct arch_dispatcher_info
{
    struct
    {
        uint8_t  present;
        uint8_t  source;
        uint16_t flags;
        uint32_t gsi;
    } acpi_iso_overrides[LEGACY_PIC_MAX_IRQS];
    uint8_t  acpi_has_ioapic;
    uint8_t  ioapic_initialized;
    uint8_t  acpi_iso_override_count;
    uint8_t  acpi_ioapic_id;
    uint32_t acpi_ioapic_gsi_base;
    uint32_t ioapic_max_redir;
    uint32_t ioapic_redir_count;
    uint64_t acpi_ioapic_base_addr;
} arch_dispatcher_info_t;

void     lapic_init(void);
void     lapic_timer_init(void);
void     lapic_eoi(void);
void     ioapic_init(void);
void     ioapic_mask_vector(uint8_t vector);
void     ioapic_unmask_vector(uint8_t vector);
uint32_t ioapic_register_device(uint32_t gsi);

#define DECL_ISR(n) void ISR##n();
DECL_ISR(0)
DECL_ISR(1)
DECL_ISR(2)
DECL_ISR(3)
DECL_ISR(4)
DECL_ISR(5)
DECL_ISR(6)
DECL_ISR(7)
DECL_ISR(8)
DECL_ISR(9)
DECL_ISR(10)
DECL_ISR(11)
DECL_ISR(12)
DECL_ISR(13)
DECL_ISR(14)
DECL_ISR(15)
DECL_ISR(16)
DECL_ISR(17)
DECL_ISR(18)
DECL_ISR(19)
DECL_ISR(20)
DECL_ISR(21)
DECL_ISR(22)
DECL_ISR(23)
DECL_ISR(24)
DECL_ISR(25)
DECL_ISR(26)
DECL_ISR(27)
DECL_ISR(28)
DECL_ISR(29)
DECL_ISR(30)
DECL_ISR(31)
DECL_ISR(32)
DECL_ISR(33)
DECL_ISR(34)
DECL_ISR(35)
DECL_ISR(36)
DECL_ISR(37)
DECL_ISR(38)
DECL_ISR(39)
DECL_ISR(40)
DECL_ISR(41)
DECL_ISR(42)
DECL_ISR(43)
DECL_ISR(44)
DECL_ISR(45)
DECL_ISR(46)
DECL_ISR(47)
DECL_ISR(48)
DECL_ISR(49)
DECL_ISR(50)
DECL_ISR(51)
DECL_ISR(52)
DECL_ISR(53)
DECL_ISR(54)
DECL_ISR(55)
DECL_ISR(56)
DECL_ISR(57)
DECL_ISR(58)
DECL_ISR(59)
DECL_ISR(60)
DECL_ISR(61)
DECL_ISR(62)
DECL_ISR(63)
DECL_ISR(64)
DECL_ISR(65)
DECL_ISR(66)
DECL_ISR(67)
DECL_ISR(68)
DECL_ISR(69)
DECL_ISR(70)
DECL_ISR(71)
DECL_ISR(72)
DECL_ISR(73)
DECL_ISR(74)
DECL_ISR(75)
DECL_ISR(76)
DECL_ISR(77)
DECL_ISR(78)
DECL_ISR(79)
DECL_ISR(80)
DECL_ISR(81)
DECL_ISR(82)
DECL_ISR(83)
DECL_ISR(84)
DECL_ISR(85)
DECL_ISR(86)
DECL_ISR(87)
DECL_ISR(88)
DECL_ISR(89)
DECL_ISR(90)
DECL_ISR(91)
DECL_ISR(92)
DECL_ISR(93)
DECL_ISR(94)
DECL_ISR(95)
DECL_ISR(96)
DECL_ISR(97)
DECL_ISR(98)
DECL_ISR(99)
DECL_ISR(100)
DECL_ISR(101)
DECL_ISR(102)
DECL_ISR(103)
DECL_ISR(104)
DECL_ISR(105)
DECL_ISR(106)
DECL_ISR(107)
DECL_ISR(108)
DECL_ISR(109)
DECL_ISR(110)
DECL_ISR(111)
DECL_ISR(112)
DECL_ISR(113)
DECL_ISR(114)
DECL_ISR(115)
DECL_ISR(116)
DECL_ISR(117)
DECL_ISR(118)
DECL_ISR(119)
DECL_ISR(120)
DECL_ISR(121)
DECL_ISR(122)
DECL_ISR(123)
DECL_ISR(124)
DECL_ISR(125)
DECL_ISR(126)
DECL_ISR(127)
DECL_ISR(128)
DECL_ISR(129)
DECL_ISR(130)
DECL_ISR(131)
DECL_ISR(132)
DECL_ISR(133)
DECL_ISR(134)
DECL_ISR(135)
DECL_ISR(136)
DECL_ISR(137)
DECL_ISR(138)
DECL_ISR(139)
DECL_ISR(140)
DECL_ISR(141)
DECL_ISR(142)
DECL_ISR(143)
DECL_ISR(144)
DECL_ISR(145)
DECL_ISR(146)
DECL_ISR(147)
DECL_ISR(148)
DECL_ISR(149)
DECL_ISR(150)
DECL_ISR(151)
DECL_ISR(152)
DECL_ISR(153)
DECL_ISR(154)
DECL_ISR(155)
DECL_ISR(156)
DECL_ISR(157)
DECL_ISR(158)
DECL_ISR(159)
DECL_ISR(160)
DECL_ISR(161)
DECL_ISR(162)
DECL_ISR(163)
DECL_ISR(164)
DECL_ISR(165)
DECL_ISR(166)
DECL_ISR(167)
DECL_ISR(168)
DECL_ISR(169)
DECL_ISR(170)
DECL_ISR(171)
DECL_ISR(172)
DECL_ISR(173)
DECL_ISR(174)
DECL_ISR(175)
DECL_ISR(176)
DECL_ISR(177)
DECL_ISR(178)
DECL_ISR(179)
DECL_ISR(180)
DECL_ISR(181)
DECL_ISR(182)
DECL_ISR(183)
DECL_ISR(184)
DECL_ISR(185)
DECL_ISR(186)
DECL_ISR(187)
DECL_ISR(188)
DECL_ISR(189)
DECL_ISR(190)
DECL_ISR(191)
DECL_ISR(192)
DECL_ISR(193)
DECL_ISR(194)
DECL_ISR(195)
DECL_ISR(196)
DECL_ISR(197)
DECL_ISR(198)
DECL_ISR(199)
DECL_ISR(200)
DECL_ISR(201)
DECL_ISR(202)
DECL_ISR(203)
DECL_ISR(204)
DECL_ISR(205)
DECL_ISR(206)
DECL_ISR(207)
DECL_ISR(208)
DECL_ISR(209)
DECL_ISR(210)
DECL_ISR(211)
DECL_ISR(212)
DECL_ISR(213)
DECL_ISR(214)
DECL_ISR(215)
DECL_ISR(216)
DECL_ISR(217)
DECL_ISR(218)
DECL_ISR(219)
DECL_ISR(220)
DECL_ISR(221)
DECL_ISR(222)
DECL_ISR(223)
DECL_ISR(224)
DECL_ISR(225)
DECL_ISR(226)
DECL_ISR(227)
DECL_ISR(228)
DECL_ISR(229)
DECL_ISR(230)
DECL_ISR(231)
DECL_ISR(232)
DECL_ISR(233)
DECL_ISR(234)
DECL_ISR(235)
DECL_ISR(236)
DECL_ISR(237)
DECL_ISR(238)
DECL_ISR(239)
DECL_ISR(240)
DECL_ISR(241)
DECL_ISR(242)
DECL_ISR(243)
DECL_ISR(244)
DECL_ISR(245)
DECL_ISR(246)
DECL_ISR(247)
DECL_ISR(248)
DECL_ISR(249)
DECL_ISR(250)
DECL_ISR(251)
DECL_ISR(252)
DECL_ISR(253)
DECL_ISR(254)
DECL_ISR(255)
#undef DECL_ISR

#endif