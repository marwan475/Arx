#ifndef VMM_H
#define VMM_H

#include <arch/arch.h>
#include <boot/boot.h>
#include <klib/klib.h>
#include <memory/metadata.h>
#include <memory/pmm.h>

#define CANONICAL_USER_BASE UINT64_C(0x0000000000000000)
#define CANONICAL_USER_END UINT64_C(0x00007fffffffffff)
#define CANONICAL_KERNEL_BASE UINT64_C(0xffff800000000000)
#define CANONICAL_KERNEL_END UINT64_C(0xffffffffffffffff)

typedef uint64_t virt_addr_t;

typedef enum virt_type
{
    VIRT_ADDR_KERNEL,
    VIRT_ADDR_USER,
} virt_type_t;

typedef struct virt_backing_block
{
    void*                      pointer;
    struct virt_backing_block* next;
} virt_backing_block_t;

typedef struct virt_region virt_region_t;

typedef struct virt_shared_page
{
    uint32_t                 references;
    void*                    pointer;
    uint64_t                 offset;
    bool                     dirty;
    bool                     shared_accounting;
    struct virt_shared_page* next;
} virt_shared_page_t;

// Resource-owned page cache. Regions and futex keys hold references independently
// of descriptors. The writer receives kernel memory and a positioned byte range.
typedef struct virt_backing_object
{
    uint32_t            references;
    uint32_t            mappings;
    uint32_t            writable_mappings; // Includes shared mappings that may gain WRITE.
    spinlock_t          lock;
    uint64_t            size;
    bool                file;
    bool                volatile_storage; // Canonical RAM contents need no external writeback.
    virt_shared_page_t* pages;
    int64_t (*write)(void*, uint64_t, const void*, uint64_t);
    void* context;
    void (*release_context)(void*);
    struct virt_backing_object* pending_next;
} virt_backing_object_t;

typedef struct virt_region
{
    virt_addr_t            start;
    virt_addr_t            end;
    size_t                 size;
    virt_type_t            type;
    bool                   allocated; // used for metadata allocations
    void*                  backing;   // Resident page bytes; null for legacy reservations.
    virt_shared_page_t*    shared;
    virt_backing_object_t* object;
    uint64_t               backing_offset;
    bool                   shared_mapping;
    uint32_t               maximum_protection;
    virt_backing_block_t*  blocks;    // vmalloc owns whole PMM allocations.
    uint32_t               protection;
    bool                   heap_page;
    virt_region_t*         next;
    virt_region_t*         prev;
} virt_region_t;

typedef struct virt_addr_space
{
    virt_type_t     type;
    phys_addr_t     pt;
    spinlock_t      lock;
    spinlock_t      operation_lock; // Serializes process-visible memory transactions.
    uint32_t        references;
    virt_region_t*  kernel_free_regions;
    virt_region_t*  kernel_used_regions;
    metadata_pool_t kernel_region_metadata_pool;
    size_t          kernel_regions_count;
    virt_region_t*  user_free_regions;
    virt_region_t*  user_used_regions;
    metadata_pool_t user_region_metadata_pool;
    size_t          user_regions_count;
} virt_addr_space_t;

virt_addr_space_t* vmm_kernel_space(void);
void              vmm_account_shared_page(virt_shared_page_t* page);
uint64_t          vmm_shared_memory_bytes(void);

void vmm_init(struct boot_info* boot_info);

// Shared kernel mapping policy; device callers retain their own verification.
uint64_t    vmm_kernel_page_flags(bool uncached, bool global);
virt_addr_t vmm_map_direct_mmio_page(phys_addr_t page, virt_addr_space_t* space);

bool vmm_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, virt_addr_space_t* space);
void vmm_unmap_page(virt_addr_t va, virt_addr_space_t* space);

bool vmm_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, virt_addr_space_t* space);
void vmm_unmap_range(virt_addr_t va_start, uint64_t size, virt_addr_space_t* space);

void vmm_protect_page(virt_addr_t va, uint64_t flags, virt_addr_space_t* space);
void vmm_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, virt_addr_space_t* space);

void vmm_switch_addr_space(virt_addr_space_t* space);

phys_addr_t vmm_virt_to_phys(virt_addr_t va, virt_addr_space_t* space);

// Resreves region in a address space but dosnt map any pages
virt_addr_t    vmm_reserve_region(virt_addr_space_t* space, size_t size, virt_type_t type);
void           vmm_free_region(virt_addr_space_t* space, virt_addr_t addr);
virt_region_t* vmm_find_region(virt_addr_space_t* space, virt_addr_t addr);
bool           vmm_reserve_page_at(virt_addr_space_t* space, virt_addr_t addr);
// Caller holds operation_lock. Read-only mappings remain read-only after fork.
bool    vmm_resolve_cow_locked(virt_addr_space_t* space, virt_addr_t address);
bool    vmm_handle_cow_fault(virt_addr_space_t* space, virt_addr_t address);
void    vmm_release_backing(virt_region_t* region);
void    vmm_retain_object(virt_backing_object_t* object);
void    vmm_put_object(virt_backing_object_t* object);
void    vmm_put_page(virt_shared_page_t* page);
int64_t vmm_flush_object(virt_backing_object_t* object);
int64_t vmm_retry_writeback(void);

#endif
