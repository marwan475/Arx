#include "layers/Resource/VirtualMemoryManager.hpp"

#include <klib/klib.h>

void VirtualMemoryManager::MapPage(virt_addr_t va, phys_addr_t pa, uint64_t flags, virt_addr_space_t* space)
{
    vmm_map_page(va, pa, flags, space);
}

void VirtualMemoryManager::UnmapPage(virt_addr_t va, virt_addr_space_t* space)
{
    vmm_unmap_page(va, space);
}

void VirtualMemoryManager::MapRange(virt_addr_t vaStart, phys_addr_t paStart, uint64_t size, uint64_t flags, virt_addr_space_t* space)
{
    vmm_map_range(vaStart, paStart, size, flags, space);
}

void VirtualMemoryManager::UnmapRange(virt_addr_t vaStart, uint64_t size, virt_addr_space_t* space)
{
    vmm_unmap_range(vaStart, size, space);
}

void VirtualMemoryManager::ProtectPage(virt_addr_t va, uint64_t flags, virt_addr_space_t* space)
{
    vmm_protect_page(va, flags, space);
}

void VirtualMemoryManager::ProtectRange(virt_addr_t vaStart, uint64_t size, uint64_t flags, virt_addr_space_t* space)
{
    vmm_protect_range(vaStart, size, flags, space);
}

void VirtualMemoryManager::SwitchAddressSpace(virt_addr_space_t* space)
{
    vmm_switch_addr_space(space);
}

phys_addr_t VirtualMemoryManager::VirtToPhys(virt_addr_t va, virt_addr_space_t* space)
{
    return vmm_virt_to_phys(va, space);
}

virt_addr_t VirtualMemoryManager::ReserveRegion(virt_addr_space_t* space, size_t size, virt_type_t type)
{
    return vmm_reserve_region(space, size, type);
}

void VirtualMemoryManager::FreeRegion(virt_addr_space_t* space, virt_addr_t addr)
{
    vmm_free_region(space, addr);
}

virt_region_t* VirtualMemoryManager::FindRegion(virt_addr_space_t* space, virt_addr_t addr)
{
    return vmm_find_region(space, addr);
}

bool VirtualMemoryManager::IsUserRangeAccessible(uintptr_t userAddress, size_t length, virt_addr_space_t* space)
{
    if (space == nullptr)
    {
        return false;
    }

    if (length == 0)
    {
        return true;
    }

    const uintptr_t userBase = static_cast<uintptr_t>(CANONICAL_USER_BASE);
    const uintptr_t userTop  = static_cast<uintptr_t>(CANONICAL_USER_END) + 1;

    if (userAddress < userBase || userAddress >= userTop)
    {
        return false;
    }

    if (length > (userTop - userAddress))
    {
        return false;
    }

    const uintptr_t pageMask = static_cast<uintptr_t>(PAGE_SIZE - 1);
    const uintptr_t lastAddr = userAddress + length - 1;
    uintptr_t page           = userAddress & ~pageMask;
    const uintptr_t lastPage = lastAddr & ~pageMask;

    while (true)
    {
        if (VirtToPhys(static_cast<virt_addr_t>(page), space) == 0)
        {
            return false;
        }

        if (page == lastPage)
        {
            break;
        }

        page += PAGE_SIZE;
    }

    return true;
}

bool VirtualMemoryManager::CopyFromUser(void* kernelDestination, uintptr_t userSource, size_t length, virt_addr_space_t* space)
{
    if (kernelDestination == nullptr)
    {
        return false;
    }

    if (length == 0)
    {
        return true;
    }

    if (!IsUserRangeAccessible(userSource, length, space))
    {
        return false;
    }

    memcpy(kernelDestination, reinterpret_cast<const void*>(userSource), length);
    return true;
}

bool VirtualMemoryManager::CopyToUser(uintptr_t userDestination, const void* kernelSource, size_t length, virt_addr_space_t* space)
{
    if (kernelSource == nullptr)
    {
        return false;
    }

    if (length == 0)
    {
        return true;
    }

    if (!IsUserRangeAccessible(userDestination, length, space))
    {
        return false;
    }

    memcpy(reinterpret_cast<void*>(userDestination), kernelSource, length);
    return true;
}

bool VirtualMemoryManager::CopyStringFromUser(char* kernelDestination, size_t destinationSize, uintptr_t userSource, virt_addr_space_t* space, size_t* outLength)
{
    if (kernelDestination == nullptr || destinationSize == 0)
    {
        return false;
    }

    size_t copied = 0;
    for (; copied < destinationSize; ++copied)
    {
        char byte = 0;
        if (!CopyFromUser(&byte, userSource + copied, sizeof(byte), space))
        {
            return false;
        }

        kernelDestination[copied] = byte;
        if (byte == '\0')
        {
            if (outLength != nullptr)
            {
                *outLength = copied;
            }
            return true;
        }
    }

    kernelDestination[destinationSize - 1] = '\0';
    return false;
}
