#ifndef LIMINE_H
#define LIMINE_H 1

#include <stdint.h>

#define LIMINE_PTR(TYPE) TYPE

#define LIMINE_REQUESTS_START_MARKER uint64_t limine_requests_start_marker[4] = {0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf, 0x785c6ed015d3e316, 0x181e920a7852b9d9}

#define LIMINE_REQUESTS_END_MARKER uint64_t limine_requests_end_marker[2] = {0xadc0e0531bb10d03, 0x9572709f31764c62}

#define LIMINE_BASE_REVISION(N) uint64_t limine_base_revision[3] = {0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, (N)}

#define LIMINE_BASE_REVISION_SUPPORTED (limine_base_revision[2] == 0)

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b

#define LIMINE_FRAMEBUFFER_REQUEST {LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75, 0xa3148604f6fab11b}

#define LIMINE_FRAMEBUFFER_RGB 1

struct limine_video_mode
{
    uint64_t pitch;
    uint64_t width;
    uint64_t height;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
};

struct limine_framebuffer
{
    LIMINE_PTR(void*) address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
    uint8_t  unused[7];
    uint64_t edid_size;
    LIMINE_PTR(void*) edid;
    uint64_t mode_count;
    LIMINE_PTR(struct limine_video_mode**) modes;
};

struct limine_framebuffer_response
{
    uint64_t revision;
    uint64_t framebuffer_count;
    LIMINE_PTR(struct limine_framebuffer**) framebuffers;
};

struct limine_framebuffer_request
{
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_framebuffer_response*) response;
};

#define LIMINE_MEMMAP_REQUEST {LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62}

#define LIMINE_MEMMAP_USABLE 0
#define LIMINE_MEMMAP_RESERVED 1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE 2
#define LIMINE_MEMMAP_ACPI_NVS 3
#define LIMINE_MEMMAP_BAD_MEMORY 4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES 6
#define LIMINE_MEMMAP_FRAMEBUFFER 7

struct limine_memmap_entry
{
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response
{
    uint64_t revision;
    uint64_t entry_count;
    LIMINE_PTR(struct limine_memmap_entry**) entries;
};

struct limine_memmap_request
{
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_memmap_response*) response;
};

#define LIMINE_HHDM_REQUEST {LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852, 0x63984e959a98244b}

struct limine_hhdm_response
{
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request
{
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_hhdm_response*) response;
};

#define LIMINE_PAGING_MODE_REQUEST {LIMINE_COMMON_MAGIC, 0x95c1a0edab0944cb, 0xa4e5cb3842f7488a}

#if defined(__x86_64__) || defined(__i386__)
#define LIMINE_PAGING_MODE_X86_64_4LVL 0
#define LIMINE_PAGING_MODE_X86_64_5LVL 1
#define LIMINE_PAGING_MODE_MIN LIMINE_PAGING_MODE_X86_64_4LVL
#define LIMINE_PAGING_MODE_DEFAULT LIMINE_PAGING_MODE_X86_64_4LVL
#elif defined(__aarch64__)
#define LIMINE_PAGING_MODE_AARCH64_4LVL 0
#define LIMINE_PAGING_MODE_AARCH64_5LVL 1
#define LIMINE_PAGING_MODE_MIN LIMINE_PAGING_MODE_AARCH64_4LVL
#define LIMINE_PAGING_MODE_DEFAULT LIMINE_PAGING_MODE_AARCH64_4LVL
#else
#error Unsupported architecture for Limine paging mode definitions
#endif

struct limine_paging_mode_response
{
    uint64_t revision;
    uint64_t mode;
};

struct limine_paging_mode_request
{
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_paging_mode_response*) response;
    uint64_t mode;
    uint64_t max_mode;
    uint64_t min_mode;
};

#define LIMINE_SMP_REQUEST {LIMINE_COMMON_MAGIC, 0x95a67b819a1b857e, 0xa0b61b723b6a73e0}

struct limine_smp_info;

typedef void (*limine_goto_address)(struct limine_smp_info*);

#if defined(__x86_64__) || defined(__i386__)
struct limine_smp_info
{
    uint32_t            processor_id;
    uint32_t            lapic_id;
    uint64_t            reserved;
    limine_goto_address goto_address;
    uint64_t            extra_argument;
};

struct limine_smp_response
{
    uint64_t revision;
    uint32_t flags;
    uint32_t bsp_lapic_id;
    uint64_t cpu_count;
    LIMINE_PTR(struct limine_smp_info**) cpus;
};
#elif defined(__aarch64__)
struct limine_smp_info
{
    uint32_t            processor_id;
    uint32_t            reserved1;
    uint64_t            mpidr;
    uint64_t            reserved;
    limine_goto_address goto_address;
    uint64_t            extra_argument;
};

struct limine_smp_response
{
    uint64_t revision;
    uint64_t flags;
    uint64_t bsp_mpidr;
    uint64_t cpu_count;
    LIMINE_PTR(struct limine_smp_info**) cpus;
};
#else
#error Unsupported architecture for Limine SMP structures
#endif

struct limine_smp_request
{
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_smp_response*) response;
    uint64_t flags;
};

#endif
