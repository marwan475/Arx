#pragma once

#ifdef __cplusplus
extern "C"
{
#include <memory/vmm.h>
}

#endif

class VirtualMemoryManager
{
public:
    virt_addr_space_t* CreateUserAddressSpace();
    void               DestroyUserAddressSpace(virt_addr_space_t* space);
    // Transaction caller holds space->operation_lock.
    bool IsPageAvailable(virt_addr_space_t* space, uint64_t address);
    bool AllocateAnonymousPage(virt_addr_space_t* space, uint64_t address, uint32_t protection, bool heap);
    void ReleaseAnonymousPage(virt_addr_space_t* space, uint64_t address);
    void ProtectAnonymousPage(virt_addr_space_t* space, uint64_t address, uint32_t protection);
    void DiscardAnonymousPage(virt_addr_space_t* space, uint64_t address);
    void ReleaseAnonymousPages(virt_addr_space_t* space);
    bool MapPage(virt_addr_t va, phys_addr_t pa, uint64_t flags, virt_addr_space_t* space);
    void UnmapPage(virt_addr_t va, virt_addr_space_t* space);

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
