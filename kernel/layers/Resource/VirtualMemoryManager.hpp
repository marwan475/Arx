#pragma once

#include <debug/kernel/stats/VirtualMemoryStats.hpp>

#ifdef __cplusplus
extern "C"
{
#include <memory/vmm.h>
}

#endif

class VirtualMemoryManager
{
public:
    ARX_DEBUG_ONLY(VirtualMemoryStats DebugStats = {};)
    virt_addr_space_t* CreateUserAddressSpace();
    virt_addr_space_t* CloneUserAddressSpace(virt_addr_space_t* source);
    void               DestroyUserAddressSpace(virt_addr_space_t* space);
    // Transaction caller holds space->operation_lock.
    bool                   IsPageAvailable(virt_addr_space_t* space, uint64_t address);
    bool                   AllocateAnonymousPage(virt_addr_space_t* space, uint64_t address, uint32_t protection, bool heap);
    virt_backing_object_t* CreateBacking(uint64_t size, bool file = false);
    static void            RetainBacking(virt_backing_object_t* object);
    static void            ReleaseBacking(virt_backing_object_t* object);
    static int64_t         FlushBacking(virt_backing_object_t* object);
    static int64_t         RetryWriteback();
    // Caller serializes file I/O; page-cache allocation is internally locked.
    virt_shared_page_t* GetBackingPage(virt_backing_object_t* object, uint64_t offset, bool create = true);
    int64_t             ResizeVolatileBacking(virt_backing_object_t* object, uint64_t size);
    // Private file offsets beyond EOF reserve a page without resident backing.
    // The reservation retains object ownership until replacement or unmap.
    bool                MapBackingPage(virt_addr_space_t* space, uint64_t address, virt_backing_object_t* object, uint64_t offset, uint32_t protection, bool shared, uint32_t maximumProtection);
    int64_t             ReleaseAnonymousPage(virt_addr_space_t* space, uint64_t address, bool force = false);
    void                ProtectAnonymousPage(virt_addr_space_t* space, uint64_t address, uint32_t protection);
    bool                DiscardAnonymousPage(virt_addr_space_t* space, uint64_t address);
    void                ReleaseAnonymousPages(virt_addr_space_t* space);
    bool                MapPage(virt_addr_t va, phys_addr_t pa, uint64_t flags, virt_addr_space_t* space);
    void                UnmapPage(virt_addr_t va, virt_addr_space_t* space);

    bool MapRange(virt_addr_t vaStart, phys_addr_t paStart, uint64_t size, uint64_t flags, virt_addr_space_t* space);
    void UnmapRange(virt_addr_t vaStart, uint64_t size, virt_addr_space_t* space);

    void ProtectPage(virt_addr_t va, uint64_t flags, virt_addr_space_t* space);
    void ProtectRange(virt_addr_t vaStart, uint64_t size, uint64_t flags, virt_addr_space_t* space);

    void SwitchAddressSpace(virt_addr_space_t* space);

    phys_addr_t VirtToPhys(virt_addr_t va, virt_addr_space_t* space);

    virt_addr_t    ReserveRegion(virt_addr_space_t* space, size_t size, virt_type_t type);
    void           FreeRegion(virt_addr_space_t* space, virt_addr_t addr);
    virt_region_t* FindRegion(virt_addr_space_t* space, virt_addr_t addr);
};
