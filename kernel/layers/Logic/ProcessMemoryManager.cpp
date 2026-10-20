#include "layers/Logic/ProcessController.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"

#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <klib/klib.h>
#include <debug/kernel/stats/VirtualMemoryStats.hpp>

namespace
{
struct MappingBackingGuard
{
    virt_backing_object_t* object   = nullptr;
    bool                   shared   = false;
    bool                   reserved = false;
    uint32_t               maximum  = 7;
    ~MappingBackingGuard()
    {
        if (object == nullptr)
        {
            return;
        }

        if (reserved)
        {
            VirtualFileSystem::ReleaseMappingBacking(object, shared, maximum);
        }
        else
        {
            VirtualMemoryManager::ReleaseBacking(object);
        }
    }
};
static bool memory_range_end(uint64_t address, uint64_t length, uint64_t* outEnd)
{
    if (!range_within_u64(address, length, CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return false;
    }

    *outEnd = align_up(address + length, PAGE_SIZE);
    return *outEnd <= CANONICAL_USER_END + 1;
}

static bool owns_anonymous_page(VirtualMemoryManager* virtualMemoryManager, virt_addr_space_t* space, uint64_t address)
{
    virt_region_t* region = virtualMemoryManager->FindRegion(space, address);
    return region != nullptr && (region->backing != nullptr || region->object != nullptr);
}

static bool memory_range_available(VirtualMemoryManager* virtualMemoryManager, virt_addr_space_t* space, uint64_t address, uint64_t length)
{
    uint64_t end = 0;

    if (address == 0 || !memory_range_end(address, length, &end))
    {
        return false;
    }

    for (uint64_t page = address; page < end; page += PAGE_SIZE)
    {
        if (!virtualMemoryManager->IsPageAvailable(space, page))
        {
            return false;
        }
    }

    return true;
}

} // namespace

process_t* ProcessController::BeginMemoryOperation()
{
    if (Resources == nullptr || Resources->processManager == nullptr || Resources->virtualMemoryManager == nullptr)
    {
        return nullptr;
    }

    cpu_preemption_disable();
    process_t* process = Resources->processManager->AcquireCurrentMemoryProcess(false);

    if (process != nullptr)
    {
        spinlock_acquire(&process->addressSpace->operation_lock);
    }
    else
    {
        cpu_preemption_enable();
    }

    return process;
}

uint64_t ProcessController::EndMemoryOperation(process_t* process, uint64_t result)
{
    spinlock_release(&process->addressSpace->operation_lock);
    Resources->processManager->ReleaseMemoryProcess(process);
    cpu_preemption_enable();
    return result;
}

uint64_t ProcessController::MapProcessMemory(uint64_t address, uint64_t length, uint32_t protection, uint32_t flags, int fd, uint64_t offset)
{
    ARX_MEMORY_SCOPE(DebugStats.mapMemory, 2);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    process_t* process = BeginMemoryOperation();

    if (process == nullptr)
    {
        return ARX_MEMORY_RETURN(LINUX_ESRCH);
    }

    if ((flags & 3) != 1 && (flags & 3) != 2)
    {
        return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EOPNOTSUPP));
    }

    MappingBackingGuard backing;
    backing.shared = (flags & 3) == 1;
    if ((flags & 0x20) == 0)
    {
        FileDescriptorGuard guard(process);
        auto* descriptor = fd < 0 ? nullptr : Resources->processManager->GetFileDescriptor(process, fd);

        if (descriptor == nullptr || descriptor->file == nullptr)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EBADF));
        }

        if (Logic == nullptr || Logic->virtualFileSystem == nullptr)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_ENOSYS));
        }

        const int64_t status = Logic->virtualFileSystem->AcquireMappingBacking(static_cast<file_t*>(descriptor->file), offset, align_up(length, PAGE_SIZE), protection, backing.shared, &backing.object, &backing.maximum);

        if (status != 0)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, status));
        }

        backing.reserved = true;
    }
    else if (backing.shared)
    {
        offset         = 0;
        backing.object = Resources->virtualMemoryManager->CreateBacking(align_up(length, PAGE_SIZE));

        if (backing.object == nullptr)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_ENOMEM));
        }
    }

    virt_addr_space_t*    space                = process->addressSpace;
    VirtualMemoryManager* virtualMemoryManager = Resources->virtualMemoryManager;
    length                                     = align_up(length, PAGE_SIZE);
    address                                    = align_down(address, PAGE_SIZE);
    const bool fixed                           = (flags & (0x10 | 0x100000)) != 0;

    if (fixed)
    {
        if (address == 0)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, static_cast<uint64_t>(-1)));
        }

        for (uint64_t page = address; page < address + length; page += PAGE_SIZE)
        {
            if (virtualMemoryManager->IsPageAvailable(space, page))
            {
                continue;
            }

            if (virtualMemoryManager->FindRegion(space, page) == nullptr && virtualMemoryManager->VirtToPhys(page, space) == 0)
            {
                return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_ENOMEM));
            }

            if ((flags & 0x100000) != 0)
            {
                return ARX_MEMORY_RETURN(EndMemoryOperation(process, static_cast<uint64_t>(-17)));
            }

            if (!owns_anonymous_page(virtualMemoryManager, space, page))
            {
                // Legacy ELF/stack allocations have no releasable backing metadata.
                return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EOPNOTSUPP));
            }
        }

        // MAP_FIXED replacement is destructive. Failure removes only the new range.

        for (uint64_t page = address; page < address + length; page += PAGE_SIZE)
        {
            const int64_t status = virtualMemoryManager->ReleaseAnonymousPage(space, page);

            if (status != 0)
            {
                return ARX_MEMORY_RETURN(EndMemoryOperation(process, status));
            }
        }
    }
    else if (!memory_range_available(virtualMemoryManager, space, address, length))
    {
        address = 0;

        for (virt_region_t* region = space->user_free_regions; region != nullptr && address == 0; region = region->next)
        {
            uint64_t candidate = align_up(region->start < 0x10000 ? 0x10000 : region->start, PAGE_SIZE);

            while (candidate < region->end && length <= region->end - candidate)
            {
                if (memory_range_available(virtualMemoryManager, space, candidate, length))
                {
                    address = candidate;
                    break;
                }

                candidate += PAGE_SIZE;
            }
        }

        if (address == 0)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_ENOMEM));
        }
    }

    uint64_t page = address;

    for (; page < address + length; page += PAGE_SIZE)
    {
        if (!(backing.object != nullptr ? virtualMemoryManager->MapBackingPage(space, page, backing.object, offset + page - address, protection, backing.shared, backing.maximum) : virtualMemoryManager->AllocateAnonymousPage(space, page, protection, false)))
        {
            break;
        }
    }

    if (page != address + length)
    {
        for (uint64_t rollback = address; rollback < page; rollback += PAGE_SIZE)
        {
            virtualMemoryManager->ReleaseAnonymousPage(space, rollback, true);
        }

        return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_ENOMEM));
    }

    return ARX_MEMORY_RETURN(EndMemoryOperation(process, address));
}

uint64_t ProcessController::UnmapProcessMemory(uint64_t address, uint64_t length)
{
    ARX_MEMORY_SCOPE(DebugStats.unmapMemory, 2);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    process_t* process = BeginMemoryOperation();

    if (process == nullptr)
    {
        return ARX_MEMORY_RETURN(LINUX_ESRCH);
    }

    virt_addr_space_t*    space                = process->addressSpace;
    VirtualMemoryManager* virtualMemoryManager = Resources->virtualMemoryManager;
    uint64_t              end                  = 0;

    if (!memory_range_end(address, length, &end))
    {
        return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EINVAL));
    }

    // Preflight legacy mappings so an unsupported request changes no pages.

    for (uint64_t page = address; page < end; page += PAGE_SIZE)
    {
        if (!owns_anonymous_page(virtualMemoryManager, space, page) && (virtualMemoryManager->FindRegion(space, page) != nullptr || virtualMemoryManager->VirtToPhys(page, space) != 0))
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EOPNOTSUPP));
        }
    }

    for (uint64_t page = address; page < end; page += PAGE_SIZE)
    {
        const int64_t status = virtualMemoryManager->ReleaseAnonymousPage(space, page);

        if (status != 0)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, status));
        }
    }

    return ARX_MEMORY_RETURN(EndMemoryOperation(process, 0));
}

uint64_t ProcessController::ProtectProcessMemory(uint64_t address, uint64_t length, uint32_t protection)
{
    ARX_MEMORY_SCOPE(DebugStats.protectMemory, 2);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    if ((address & (PAGE_SIZE - 1)) != 0)
    {
        return ARX_MEMORY_RETURN(LINUX_EINVAL);
    }

    process_t* process = BeginMemoryOperation();

    if (process == nullptr)
    {
        return ARX_MEMORY_RETURN(LINUX_ESRCH);
    }

    virt_addr_space_t*    space                = process->addressSpace;
    VirtualMemoryManager* virtualMemoryManager = Resources->virtualMemoryManager;
    uint64_t              end                  = 0;

    if (!memory_range_end(address, length, &end))
    {
        return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_ENOMEM));
    }

    for (uint64_t page = address; page < end; page += PAGE_SIZE)
    {
        if (!owns_anonymous_page(virtualMemoryManager, space, page))
        {
            const uint64_t error = virtualMemoryManager->VirtToPhys(page, space) != 0 ? LINUX_EOPNOTSUPP : LINUX_ENOMEM;
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, error));
        }

        if ((protection & ~virtualMemoryManager->FindRegion(space, page)->maximum_protection) != 0)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, static_cast<uint64_t>(-13)));
        }
    }

    for (uint64_t page = address; page < end; page += PAGE_SIZE)
    {
        virtualMemoryManager->ProtectAnonymousPage(space, page, protection);
    }

    return ARX_MEMORY_RETURN(EndMemoryOperation(process, 0));
}

uint64_t ProcessController::QueryProcessMemoryResidency(uint64_t address, uint64_t length, uint64_t vector)
{
    ARX_MEMORY_SCOPE(DebugStats.residency, 2);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    process_t* process = BeginMemoryOperation();

    if (process == nullptr)
    {
        return ARX_MEMORY_RETURN(LINUX_ESRCH);
    }

    virt_addr_space_t*    space                = process->addressSpace;
    VirtualMemoryManager* virtualMemoryManager = Resources->virtualMemoryManager;
    uint64_t              end                  = 0;

    if (!memory_range_end(address, length, &end))
    {
        return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_ENOMEM));
    }

    const uint64_t count = (end - address) / PAGE_SIZE;

    if (count == 0)
    {
        return ARX_MEMORY_RETURN(EndMemoryOperation(process, 0));
    }

    if (!range_within_u64(vector, count, CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EFAULT));
    }

    for (uint64_t page = align_down(vector, PAGE_SIZE); page < vector + count; page += PAGE_SIZE)
    {
        virt_region_t* region = virtualMemoryManager->FindRegion(space, page);

        if (region == nullptr || region->backing == nullptr || (region->protection & 2) == 0)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EFAULT));
        }
    }

    for (uint64_t page = address; page < end; page += PAGE_SIZE)
    {
        if (!owns_anonymous_page(virtualMemoryManager, space, page) && virtualMemoryManager->VirtToPhys(page, space) == 0)
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_ENOMEM));
        }
    }

    const user_memory_access_t access = {nullptr, nullptr, space};

    for (uint64_t page = address; page < end; page += PAGE_SIZE)
    {
        const virt_region_t* region = virtualMemoryManager->FindRegion(space, page);
        const uint8_t resident = (region != nullptr && region->backing != nullptr) || virtualMemoryManager->VirtToPhys(page, space) != 0 ? 1 : 0;

        if (!safe_write_user_locked(&access, vector++, &resident, sizeof(resident)))
        {
            return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EFAULT));
        }
    }

    return ARX_MEMORY_RETURN(EndMemoryOperation(process, 0));
}

uint64_t ProcessController::AdviseProcessMemory(uint64_t address, uint64_t length, int advice)
{
    ARX_MEMORY_SCOPE(DebugStats.adviseMemory, 2);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    if (advice < 0 || advice > 4)
    {
        return ARX_MEMORY_RETURN(LINUX_EINVAL);
    }
    process_t* process = BeginMemoryOperation();

    if (process == nullptr)
    {
        return ARX_MEMORY_RETURN(LINUX_ESRCH);
    }

    virt_addr_space_t*    space                = process->addressSpace;
    VirtualMemoryManager* virtualMemoryManager = Resources->virtualMemoryManager;
    uint64_t              end                  = 0;

    if (!memory_range_end(address, length, &end))
    {
        return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EINVAL));
    }

    // DONTNEED on an untracked mapping cannot preserve its backing semantics.

    if (advice == 4)
    {
        for (uint64_t page = address; page < end; page += PAGE_SIZE)
        {
            auto* region = virtualMemoryManager->FindRegion(space, page);

            if (region != nullptr && region->shared_mapping)
            {
                const int64_t status = VirtualMemoryManager::FlushBacking(region->object);

                if (status != 0)
                {
                    return ARX_MEMORY_RETURN(EndMemoryOperation(process, status));
                }
            }
            if (!owns_anonymous_page(virtualMemoryManager, space, page) && virtualMemoryManager->VirtToPhys(page, space) != 0)
            {
                return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_EOPNOTSUPP));
            }
        }
    }

    uint64_t result = 0;

    for (uint64_t page = address; page < end; page += PAGE_SIZE)
    {
        virt_region_t* region = virtualMemoryManager->FindRegion(space, page);

        if (region == nullptr || region->backing == nullptr)
        {
            if (virtualMemoryManager->VirtToPhys(page, space) == 0)
            {
                result = LINUX_ENOMEM;
            }

            continue;
        }

        if (advice == 4)
        {
            if (region->shared_mapping)
            {
                continue;
            } // Eager shared cache remains coherent.
            // Anonymous discard zeroes; file discard restores the canonical page.
            if (!virtualMemoryManager->DiscardAnonymousPage(space, page))
            {
                return ARX_MEMORY_RETURN(EndMemoryOperation(process, LINUX_ENOMEM));
            }
        }
    }

    return ARX_MEMORY_RETURN(EndMemoryOperation(process, result));
}

uint64_t ProcessController::ResizeProcessBreak(uint64_t address)
{
    ARX_DEBUG_ONLY(DebugOperation debugBreak(DebugStats.resizeBreak);)
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    process_t* process = BeginMemoryOperation();

    if (process == nullptr)
    {
        return Resources->processManager != nullptr ? Resources->processManager->GetCurrentProgramBreak() : 0;
    }

    virt_addr_space_t*    space                = process->addressSpace;
    VirtualMemoryManager* virtualMemoryManager = Resources->virtualMemoryManager;
    const uint64_t        oldBreak             = process->programBreak;

    if (address == 0 || process->breakBase == 0 || address < process->breakBase || address > CANONICAL_USER_END)
    {
        ARX_DEBUG_RUN(debugBreak.Finish(address == 0 || address == oldBreak ? 1 : 3));
        return EndMemoryOperation(process, oldBreak);
    }

    const uint64_t oldEnd = align_up(oldBreak, PAGE_SIZE);
    const uint64_t newEnd = align_up(address, PAGE_SIZE);

    if (newEnd > oldEnd)
    {
        if (!memory_range_available(virtualMemoryManager, space, oldEnd, newEnd - oldEnd))
        {
            ARX_DEBUG_RUN(debugBreak.Finish(3));
            return EndMemoryOperation(process, oldBreak);
        }

        uint64_t page = oldEnd;

        for (; page < newEnd; page += PAGE_SIZE)
        {
            if (!virtualMemoryManager->AllocateAnonymousPage(space, page, 3, true))
            {
                break;
            }
        }

        if (page != newEnd)
        {
            for (uint64_t rollback = oldEnd; rollback < page; rollback += PAGE_SIZE)
            {
                virtualMemoryManager->ReleaseAnonymousPage(space, rollback);
            }

            ARX_DEBUG_RUN(debugBreak.Finish(3));
            return EndMemoryOperation(process, oldBreak);
        }
    }
    else if (newEnd < oldEnd)
    {
        for (uint64_t page = newEnd; page < oldEnd; page += PAGE_SIZE)
        {
            virt_region_t* region = virtualMemoryManager->FindRegion(space, page);

            if (region != nullptr && region->backing != nullptr)
            {
                virtualMemoryManager->ReleaseAnonymousPage(space, page);
            }
        }
    }

    // Linux preserves the retained partial page on shrink/regrow.
    process->programBreak = address;
    ARX_DEBUG_RUN(debugBreak.Finish(address == oldBreak ? 1 : 0));
    return EndMemoryOperation(process, address);
}
