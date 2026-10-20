#include "layers/Logic/ElfMapper.hpp"

#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

extern "C"
{
#include <boot/boot.h>
#include <klib/klib.h>
#include <memory/vmm.h>
#include <platform.h>
}

struct __attribute__((packed)) ElfMapper::elf64_header_t
{
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t programHeaderOffset;
    uint64_t sectionHeaderOffset;
    uint32_t flags;
    uint16_t headerSize;
    uint16_t programHeaderEntrySize;
    uint16_t programHeaderCount;
    uint16_t sectionHeaderEntrySize;
    uint16_t sectionHeaderCount;
    uint16_t sectionHeaderStringIndex;
};

struct __attribute__((packed)) ElfMapper::elf64_program_header_t
{
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtualAddress;
    uint64_t physicalAddress;
    uint64_t fileSize;
    uint64_t memorySize;
    uint64_t alignment;
};

namespace
{
constexpr uint8_t ELF_MAGIC_0 = 0x7F;
constexpr uint8_t ELF_MAGIC_1 = 'E';
constexpr uint8_t ELF_MAGIC_2 = 'L';
constexpr uint8_t ELF_MAGIC_3 = 'F';

constexpr uint8_t ELF_CLASS_64        = 2;
constexpr uint8_t ELF_DATA_LSB        = 1;
constexpr uint8_t ELF_VERSION_CURRENT = 1;

constexpr uint16_t ELF_TYPE_EXEC = 2;
constexpr uint16_t ELF_TYPE_DYN  = 3;

constexpr uint16_t ELF_MACHINE_X86_64  = 62;
constexpr uint16_t ELF_MACHINE_AARCH64 = 183;

constexpr uint32_t ELF_PH_TYPE_LOAD = 1;

constexpr uint32_t ELF_PF_X = 0x1;
constexpr uint32_t ELF_PF_W = 0x2;
constexpr uint32_t ELF_PF_R = 0x4;

constexpr uint64_t ELF_MAX_PROGRAM_HEADERS = 1024;
} // namespace

ElfMapper::ElfMapper(ResourceLayerCaps* resourceLayerCaps)
{
    ResourceLayerImportCaps = resourceLayerCaps;
}

bool ElfMapper::AddWouldOverflow(uint64_t a, uint64_t b)
{
    return add_overflows_u64(a, b);
}

uint64_t ElfMapper::MinU64(uint64_t a, uint64_t b)
{
    return (a < b) ? a : b;
}

bool ElfMapper::IsPowerOfTwo(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

bool ElfMapper::ReadExactAt(file_t* file, uint64_t offset, void* buffer, uint64_t size) const
{
    if (file == nullptr || buffer == nullptr)
    {
        return false;
    }

    if (size == 0)
    {
        return true;
    }

    if (file->operations == nullptr || file->operations->Read == nullptr || file->operations->Seek == nullptr)
    {
        return false;
    }

    const uint64_t originalOffset = file->offset;

    if (file->operations->Seek(file, (int64_t) offset, 0) != (int64_t) offset)
    {
        return false;
    }

    uint64_t bytesRead = 0;

    while (bytesRead < size)
    {
        int64_t result = file->operations->Read(file, (uint8_t*) buffer + bytesRead, size - bytesRead);

        if (result <= 0)
        {
            (void) file->operations->Seek(file, (int64_t) originalOffset, 0);
            return false;
        }

        bytesRead += (uint64_t) result;
    }

    return file->operations->Seek(file, (int64_t) originalOffset, 0) == (int64_t) originalOffset;
}

bool ElfMapper::IsSupportedMachine(uint16_t machine) const
{
    return machine == arch_elf_machine();
}

bool ElfMapper::ValidateUserspaceRange(uint64_t virtualAddress, uint64_t memorySize) const
{
    if (!range_within_u64(virtualAddress, memorySize, CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return false;
    }

    if (memorySize == 0)
    {
        return true;
    }

    const uint64_t inclusiveEnd = virtualAddress + (memorySize - 1);

    const uint64_t alignedStart = align_down(virtualAddress, PAGE_SIZE);

    if (AddWouldOverflow(inclusiveEnd, 1))
    {
        return false;
    }

    const uint64_t alignedEndExclusive = align_up(inclusiveEnd + 1, PAGE_SIZE);

    if (alignedStart < CANONICAL_USER_BASE)
    {
        return false;
    }

    if (alignedEndExclusive == 0)
    {
        return false;
    }

    return alignedEndExclusive - 1 <= CANONICAL_USER_END;
}

bool ElfMapper::ValidateLoadSegment(const elf64_program_header_t& programHeader, uint64_t fileSize) const
{
    ARX_MEMORY_SCOPE(DebugStats.validate, 0);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    if (programHeader.fileSize > programHeader.memorySize)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (programHeader.fileSize > 0)
    {
        if (programHeader.offset > fileSize)
        {
            return ARX_MEMORY_RETURN(false);
        }

        if (programHeader.fileSize > fileSize - programHeader.offset)
        {
            return ARX_MEMORY_RETURN(false);
        }
    }

    if (!ValidateUserspaceRange(programHeader.virtualAddress, programHeader.memorySize))
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (programHeader.alignment != 0 && programHeader.alignment != 1)
    {
        if (!IsPowerOfTwo(programHeader.alignment))
        {
            return ARX_MEMORY_RETURN(false);
        }

        if ((programHeader.virtualAddress % programHeader.alignment) != (programHeader.offset % programHeader.alignment))
        {
            return ARX_MEMORY_RETURN(false);
        }
    }

    return ARX_MEMORY_RETURN(true);
}

bool ElfMapper::ValidateHeader(const elf64_header_t& header, uint64_t fileSize) const
{
    ARX_MEMORY_SCOPE(DebugStats.validate, 0);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    if (header.ident[0] != ELF_MAGIC_0 || header.ident[1] != ELF_MAGIC_1 || header.ident[2] != ELF_MAGIC_2 || header.ident[3] != ELF_MAGIC_3)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (header.ident[4] != ELF_CLASS_64)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (header.ident[5] != ELF_DATA_LSB)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (header.ident[6] != ELF_VERSION_CURRENT || header.version != ELF_VERSION_CURRENT)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (!IsSupportedMachine(header.machine))
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (header.type != ELF_TYPE_EXEC && header.type != ELF_TYPE_DYN)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (header.headerSize < sizeof(elf64_header_t))
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (header.programHeaderCount == 0 || header.programHeaderCount > ELF_MAX_PROGRAM_HEADERS)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (header.programHeaderEntrySize != sizeof(elf64_program_header_t))
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (header.programHeaderOffset > fileSize)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (AddWouldOverflow((uint64_t) header.programHeaderEntrySize, (uint64_t) header.programHeaderCount))
    {
        return ARX_MEMORY_RETURN(false);
    }

    const uint64_t tableSize = (uint64_t) header.programHeaderEntrySize * (uint64_t) header.programHeaderCount;

    if (tableSize > fileSize - header.programHeaderOffset)
    {
        return ARX_MEMORY_RETURN(false);
    }

    return ARX_MEMORY_RETURN(true);
}

bool ElfMapper::ParseProgramHeaders(file_t* file, const elf64_header_t& header, uint64_t fileSize, elf_metadata_t* metadata) const
{
    const uint64_t tableSize = (uint64_t) header.programHeaderEntrySize * (uint64_t) header.programHeaderCount;

    elf64_program_header_t* headers = (elf64_program_header_t*) kmalloc((size_t) tableSize);

    if (headers == nullptr)
    {
        return false;
    }

    if (!ReadExactAt(file, header.programHeaderOffset, headers, tableSize))
    {
        kfree(headers);
        return false;
    }

    uint64_t loadSegmentCount = 0;

    for (uint16_t i = 0; i < header.programHeaderCount; ++i)
    {
        if (headers[i].type == 3)
        {
            if (metadata->interpreter[0] != '\0' || headers[i].fileSize < 2 || headers[i].fileSize > sizeof(metadata->interpreter) || headers[i].offset > fileSize || headers[i].fileSize > fileSize - headers[i].offset
                || !ReadExactAt(file, headers[i].offset, metadata->interpreter, headers[i].fileSize) || metadata->interpreter[headers[i].fileSize - 1] != '\0')
            {
                kfree(headers);
                return false;
            }
        }

        if (headers[i].type == 0x6474e551)
        {
            metadata->executableStack = (headers[i].flags & ELF_PF_X) != 0;
        }

        if (headers[i].type == ELF_PH_TYPE_LOAD)
        {
            loadSegmentCount++;
        }
    }

    if (loadSegmentCount == 0)
    {
        kfree(headers);
        return false;
    }

    elf_segment_metadata_t* segments = (elf_segment_metadata_t*) kmalloc(sizeof(elf_segment_metadata_t) * (size_t) loadSegmentCount);

    if (segments == nullptr)
    {
        kfree(headers);
        return false;
    }

    memset(segments, 0, sizeof(elf_segment_metadata_t) * (size_t) loadSegmentCount);

    uint64_t outIndex = 0;

    for (uint16_t i = 0; i < header.programHeaderCount; ++i)
    {
        if (headers[i].type != ELF_PH_TYPE_LOAD)
        {
            continue;
        }

        if (!ValidateLoadSegment(headers[i], fileSize))
        {
            kfree(segments);
            kfree(headers);
            return false;
        }

        segments[outIndex].virtualAddress = headers[i].virtualAddress;
        segments[outIndex].fileOffset     = headers[i].offset;
        segments[outIndex].fileSize       = headers[i].fileSize;
        segments[outIndex].memorySize     = headers[i].memorySize;
        segments[outIndex].alignment      = headers[i].alignment;
        segments[outIndex].flags          = headers[i].flags & (ELF_PF_R | ELF_PF_W | ELF_PF_X);
        segments[outIndex].mappedAddress  = 0;
        segments[outIndex].mapped         = false;
        outIndex++;
    }

    metadata->segments     = segments;
    metadata->segmentCount = loadSegmentCount;

    kfree(headers);
    return true;
}

bool ElfMapper::ReadElf(file_t* file, elf_metadata_t* metadata)
{
    ARX_MEMORY_SCOPE(DebugStats.parse, 0);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    if (file == nullptr || metadata == nullptr)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (file->inode == nullptr)
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (metadata->segments != nullptr)
    {
        kfree(metadata->segments);
        metadata->segments = nullptr;
    }

    memset(metadata, 0, sizeof(*metadata));

    const uint64_t fileSize = file->inode->size;

    if (fileSize < sizeof(elf64_header_t))
    {
        return ARX_MEMORY_RETURN(false);
    }

    elf64_header_t header = {};

    if (!ReadExactAt(file, 0, &header, sizeof(header)))
    {
        return ARX_MEMORY_RETURN(false);
    }

    if (!ValidateHeader(header, fileSize))
    {
        return ARX_MEMORY_RETURN(false);
    }

    metadata->type                   = header.type;
    metadata->machine                = header.machine;
    metadata->originalEntryPoint     = header.entry;
    metadata->entryPoint             = header.entry;
    metadata->loadBias               = 0;
    metadata->programHeaderOffset    = header.programHeaderOffset;
    metadata->programHeaderEntrySize = header.programHeaderEntrySize;
    metadata->programHeaderCount     = header.programHeaderCount;
    metadata->segments               = nullptr;
    metadata->segmentCount           = 0;
    metadata->executableLoaded       = false;

    if (!ParseProgramHeaders(file, header, fileSize, metadata))
    {
        memset(metadata, 0, sizeof(*metadata));
        return ARX_MEMORY_RETURN(false);
    }

    return ARX_MEMORY_RETURN(true);
}

bool ElfMapper::LoadExecutable(process_t* process, file_t* file, elf_metadata_t* metadata)
{
    ARX_MEMORY_SCOPE(DebugStats.loadExecutable, 0);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    return ARX_MEMORY_RETURN(LoadImage(process, file, metadata, metadata != nullptr ? metadata->loadBias : 0, true));
}

bool ElfMapper::LoadLibrary(process_t* process, file_t* file, elf_metadata_t* metadata, uint64_t requestedLoadBias)
{
    ARX_MEMORY_SCOPE(DebugStats.loadLibrary, 0);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    if (metadata == nullptr || metadata->type != ELF_TYPE_DYN)
    {
        return ARX_MEMORY_RETURN(false);
    }

    return ARX_MEMORY_RETURN(LoadImage(process, file, metadata, requestedLoadBias, false));
}

bool ElfMapper::LoadImage(process_t* process, file_t* file, elf_metadata_t* metadata, uint64_t requestedLoadBias, bool initializeHeap)
{
    ARX_MEMORY_SCOPE(DebugStats.mapImage, 0);
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    if (process == nullptr || file == nullptr || metadata == nullptr)
    {
        return false;
    }

    if (process->addressSpace == nullptr)
    {
        return false;
    }

    if (metadata->segments == nullptr || metadata->segmentCount == 0)
    {
        return false;
    }

    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->virtualMemoryManager == nullptr || ResourceLayerImportCaps->physicalMemoryManager == nullptr)
    {
        return false;
    }

    if (metadata->type != ELF_TYPE_EXEC && metadata->type != ELF_TYPE_DYN)
    {
        return false;
    }

    VirtualMemoryManager* virtualMemoryManager = ResourceLayerImportCaps->virtualMemoryManager;

    // Acquire file backing before the address-space transaction.
    // Synthetic/read-only backends without GetBacking retain the
    // anonymous-copy path; real file backends must not bypass cache failures.
    struct backing_guard_t
    {
        virt_backing_object_t* object  = nullptr;
        uint32_t               maximum = 7;
        ~backing_guard_t()
        {
            if (object != nullptr)
            {
                VirtualFileSystem::ReleaseMappingBacking(object, false, maximum);
            }
        }
    } backing;

    if (file->operations != nullptr && file->operations->GetBacking != nullptr)
    {
        if (file->owner == nullptr || file->inode == nullptr || file->inode->size == 0 || file->owner->AcquireMappingBacking(file, 0, file->inode->size, 1, false, &backing.object, &backing.maximum) != 0)
        {
            return false;
        }
    }

    struct transaction_guard_t
    {
        virt_addr_space_t* space;
        explicit transaction_guard_t(virt_addr_space_t* addressSpace) : space(addressSpace)
        {
            spinlock_acquire_preempt(&space->operation_lock);
        }
        ~transaction_guard_t()
        {
            spinlock_release_preempt(&space->operation_lock);
        }
    } transaction(process->addressSpace);

    uint64_t minimum   = UINT64_MAX;
    uint64_t maximum   = 0;
    uint64_t alignment = PAGE_SIZE;

    for (uint64_t index = 0; index < metadata->segmentCount; ++index)
    {
        const elf_segment_metadata_t& segment = metadata->segments[index];

        if (segment.fileSize > segment.memorySize || AddWouldOverflow(segment.virtualAddress, segment.memorySize) || (segment.alignment > 1 && !IsPowerOfTwo(segment.alignment))
            || (backing.object != nullptr && (segment.fileOffset > backing.object->size || segment.fileSize > backing.object->size - segment.fileOffset)))
        {
            return false;
        }

        if (segment.alignment > alignment)
        {
            alignment = segment.alignment;
        }

        if (segment.memorySize == 0)
        {
            continue;
        }

        minimum      = MinU64(minimum, align_down(segment.virtualAddress, PAGE_SIZE));
        uint64_t end = segment.virtualAddress + segment.memorySize;

        if (end > CANONICAL_USER_END)
        {
            return false;
        }

        end     = align_up(end, PAGE_SIZE);
        maximum = end > maximum ? end : maximum;
    }

    if (minimum == UINT64_MAX || alignment > CANONICAL_USER_END)
    {
        return false;
    }

    const bool automatic = metadata->type == ELF_TYPE_DYN && requestedLoadBias == 0;
    uint64_t   bias      = metadata->type == ELF_TYPE_EXEC ? 0 : automatic ? align_up(UINT64_C(0x40000000), alignment) : requestedLoadBias;

    if (bias % alignment != 0)
    {
        return false;
    }

    // Check the entire image span, including holes, and skip occupied regions
    // rather than walking every page of an exhausted virtual address space.
    for (;;)
    {
        if (maximum > CANONICAL_USER_END + 1 || bias > CANONICAL_USER_END + 1 - maximum || bias + minimum < 0x10000)
        {
            return false;
        }

        bool     available = true;
        uint64_t next      = bias;

        for (uint64_t address = bias + minimum; address < bias + maximum; address += PAGE_SIZE)
        {
            if (!virtualMemoryManager->IsPageAvailable(process->addressSpace, address))
            {
                available             = false;
                virt_region_t* region = virtualMemoryManager->FindRegion(process->addressSpace, address);
                uint64_t       end    = region != nullptr ? region->end : address + PAGE_SIZE;
                next                  = align_up(end - minimum, alignment);
                break;
            }
        }

        if (available)
        {
            break;
        }

        if (!automatic || next <= bias)
        {
            return false;
        }

        bias = next;
    }

    if (metadata->originalEntryPoint > CANONICAL_USER_END || bias > CANONICAL_USER_END - metadata->originalEntryPoint)
    {
        return false;
    }

    metadata->loadBias   = bias;
    metadata->entryPoint = metadata->originalEntryPoint;

    struct mapped_page_state_t
    {
        uint64_t virtualAddress;
        void*    pagePointer;
        uint32_t combinedFlags;
        bool     fileBacked;
    };

    uint64_t maxPageCount = 0;

    for (uint64_t i = 0; i < metadata->segmentCount; ++i)
    {
        elf_segment_metadata_t& segment = metadata->segments[i];
        segment.mapped                  = false;
        segment.mappedAddress           = 0;

        if (segment.memorySize == 0)
        {
            continue;
        }

        uint64_t mappedAddress = 0;

        if (AddWouldOverflow(metadata->loadBias, segment.virtualAddress))
        {
            return false;
        }

        mappedAddress = metadata->loadBias + segment.virtualAddress;

        if (!ValidateUserspaceRange(mappedAddress, segment.memorySize) || mappedAddress < 0x10000)
        {
            return false;
        }

        if (AddWouldOverflow(mappedAddress, segment.memorySize - 1))
        {
            return false;
        }

        const uint64_t mapStart = align_down(mappedAddress, PAGE_SIZE);
        const uint64_t mapEnd   = align_up(mappedAddress + segment.memorySize, PAGE_SIZE);
        const uint64_t mapSize  = mapEnd - mapStart;

        if (mapSize % PAGE_SIZE != 0)
        {
            return false;
        }

        if (mapSize / PAGE_SIZE > SIZE_MAX / sizeof(mapped_page_state_t) - maxPageCount)
        {
            return false;
        }

        maxPageCount += mapSize / PAGE_SIZE;
    }

    mapped_page_state_t* mappedPages = nullptr;

    if (maxPageCount > 0)
    {
        mappedPages = (mapped_page_state_t*) kmalloc(sizeof(mapped_page_state_t) * (size_t) maxPageCount);

        if (mappedPages == nullptr)
        {
            return false;
        }

        memset(mappedPages, 0, sizeof(mapped_page_state_t) * (size_t) maxPageCount);
    }

    uint8_t* scratch = (uint8_t*) kmalloc(PAGE_SIZE);

    if (scratch == nullptr)
    {
        if (mappedPages != nullptr)
        {
            kfree(mappedPages);
        }

        return false;
    }

    uint64_t mappedPageCount = 0;

    auto findMappedPageIndex = [&](uint64_t pageVirtualAddress) -> int64_t
    {
        for (uint64_t index = 0; index < mappedPageCount; ++index)
        {
            if (mappedPages[index].virtualAddress == pageVirtualAddress)
            {
                return (int64_t) index;
            }
        }

        return -1;
    };

    metadata->executableLoaded = false;

    for (uint64_t segmentIndex = 0; segmentIndex < metadata->segmentCount; ++segmentIndex)
    {
        elf_segment_metadata_t& segment = metadata->segments[segmentIndex];

        if (AddWouldOverflow(metadata->loadBias, segment.virtualAddress))
        {
            goto fail;
        }

        const uint64_t mappedAddress = metadata->loadBias + segment.virtualAddress;
        segment.mappedAddress        = mappedAddress;

        if (segment.memorySize == 0)
        {
            segment.mapped = true;
            continue;
        }

        const uint64_t mapStart = align_down(mappedAddress, PAGE_SIZE);
        const uint64_t mapEnd   = align_up(mappedAddress + segment.memorySize, PAGE_SIZE);

        for (uint64_t pageAddress = mapStart; pageAddress < mapEnd; pageAddress += PAGE_SIZE)
        {
            int64_t existingIndex = findMappedPageIndex(pageAddress);

            if (existingIndex >= 0)
            {
                mappedPages[(uint64_t) existingIndex].combinedFlags |= segment.flags;
                continue;
            }

            if (virtualMemoryManager->VirtToPhys(pageAddress, process->addressSpace) != 0)
            {
                goto fail;
            }

            bool     shareable     = backing.object != nullptr;
            bool     hasFileBytes  = false;
            uint64_t backingOffset = 0;
            uint32_t combinedFlags = 0;

            // A page can use canonical file backing only if every contributing
            // segment agrees on its file offset and no segment needs BSS zeros.
            // Mixed/unaligned pages are composed privately below.
            for (uint64_t index = 0; index < metadata->segmentCount; ++index)
            {
                const elf_segment_metadata_t& other = metadata->segments[index];
                const uint64_t                start = metadata->loadBias + other.virtualAddress;
                const uint64_t                end   = start + other.memorySize;

                if (other.memorySize == 0 || start >= pageAddress + PAGE_SIZE || end <= pageAddress)
                {
                    continue;
                }

                combinedFlags |= other.flags;
                const uint64_t fileEnd = start + other.fileSize;

                if (other.memorySize > other.fileSize && fileEnd < pageAddress + PAGE_SIZE && end > pageAddress)
                {
                    shareable = false;
                }

                if (other.fileSize == 0 || fileEnd <= pageAddress)
                {
                    continue;
                }

                if ((start & (PAGE_SIZE - 1)) != (other.fileOffset & (PAGE_SIZE - 1)))
                {
                    shareable = false;
                    continue;
                }

                const uint64_t offset = align_down(other.fileOffset, PAGE_SIZE) + pageAddress - align_down(start, PAGE_SIZE);

                if (hasFileBytes && offset != backingOffset)
                {
                    shareable = false;
                }

                backingOffset = offset;
                hasFileBytes  = true;
            }

            shareable                 = shareable && hasFileBytes;
            const uint32_t protection = 1U | ((combinedFlags & ELF_PF_W) != 0 ? 2U : 0U) | ((combinedFlags & ELF_PF_X) != 0 ? 4U : 0U);

            if (shareable ? !virtualMemoryManager->MapBackingPage(process->addressSpace, pageAddress, backing.object, backingOffset, protection, false, backing.maximum) : !virtualMemoryManager->AllocateAnonymousPage(process->addressSpace, pageAddress, 3, false))
            {
                goto fail;
            }

            void* pageBuffer                            = virtualMemoryManager->FindRegion(process->addressSpace, pageAddress)->backing;
            mappedPages[mappedPageCount].virtualAddress = pageAddress;
            mappedPages[mappedPageCount].pagePointer    = pageBuffer;
            mappedPages[mappedPageCount].combinedFlags  = combinedFlags;
            mappedPages[mappedPageCount].fileBacked     = shareable;
            mappedPageCount++;
        }

        uint64_t sourceOffset = segment.fileOffset;
        uint64_t destination  = mappedAddress;
        uint64_t bytesToCopy  = segment.fileSize;

        while (bytesToCopy > 0)
        {
            const uint64_t pageOffset = destination & (PAGE_SIZE - 1);
            const uint64_t chunkSize  = MinU64(PAGE_SIZE - pageOffset, bytesToCopy);

            const uint64_t pageAddress = align_down(destination, PAGE_SIZE);
            int64_t        pageIndex   = findMappedPageIndex(pageAddress);

            if (pageIndex < 0)
            {
                goto fail;
            }

            if (!mappedPages[(uint64_t) pageIndex].fileBacked)
            {
                if (!ReadExactAt(file, sourceOffset, scratch, chunkSize))
                {
                    goto fail;
                }

                memcpy((uint8_t*) mappedPages[(uint64_t) pageIndex].pagePointer + pageOffset, scratch, (size_t) chunkSize);
            }

            destination += chunkSize;
            sourceOffset += chunkSize;
            bytesToCopy -= chunkSize;
        }

        uint64_t zeroDestination = mappedAddress + segment.fileSize;
        uint64_t zeroSize        = segment.memorySize - segment.fileSize;

        while (zeroSize > 0)
        {
            const uint64_t pageOffset = zeroDestination & (PAGE_SIZE - 1);
            const uint64_t chunkSize  = MinU64(PAGE_SIZE - pageOffset, zeroSize);

            const uint64_t pageAddress = align_down(zeroDestination, PAGE_SIZE);
            int64_t        pageIndex   = findMappedPageIndex(pageAddress);

            if (pageIndex < 0)
            {
                goto fail;
            }

            memset((uint8_t*) mappedPages[(uint64_t) pageIndex].pagePointer + pageOffset, 0, (size_t) chunkSize);

            zeroDestination += chunkSize;
            zeroSize -= chunkSize;
        }

        segment.mapped = true;
    }

    for (uint64_t pageIndex = 0; pageIndex < mappedPageCount; ++pageIndex)
    {
        uint64_t protectionFlags = 0;
        ARCH_PAGE_FLAGS_INIT(protectionFlags);
        ARCH_PAGE_FLAG_SET_READ(protectionFlags);
        ARCH_PAGE_FLAG_SET_USER(protectionFlags);

        if ((mappedPages[pageIndex].combinedFlags & ELF_PF_W) != 0)
        {
            ARCH_PAGE_FLAG_SET_WRITE(protectionFlags);
        }

        if ((mappedPages[pageIndex].combinedFlags & ELF_PF_X) != 0)
        {
            ARCH_PAGE_FLAG_SET_EXEC(protectionFlags);
        }

        const uint32_t protection = 1U | ((mappedPages[pageIndex].combinedFlags & ELF_PF_W) != 0 ? 2U : 0U) | ((mappedPages[pageIndex].combinedFlags & ELF_PF_X) != 0 ? 4U : 0U);
        virtualMemoryManager->ProtectAnonymousPage(process->addressSpace, mappedPages[pageIndex].virtualAddress, protection);
    }

    metadata->entryPoint           = metadata->loadBias + metadata->originalEntryPoint;
    metadata->programHeaderAddress = 0;

    for (uint64_t index = 0; index < metadata->segmentCount; ++index)
    {
        const elf_segment_metadata_t& segment   = metadata->segments[index];
        const uint64_t                tableSize = static_cast<uint64_t>(metadata->programHeaderCount) * metadata->programHeaderEntrySize;

        if (metadata->programHeaderOffset >= segment.fileOffset && metadata->programHeaderOffset - segment.fileOffset <= segment.fileSize && tableSize <= segment.fileSize - (metadata->programHeaderOffset - segment.fileOffset))
        {
            metadata->programHeaderAddress = segment.mappedAddress + metadata->programHeaderOffset - segment.fileOffset;
            break;
        }
    }
    metadata->executableLoaded = true;
    if (initializeHeap)
    {
        uint64_t heapEnd = 0;

        for (uint64_t i = 0; i < metadata->segmentCount; ++i)
        {
            const auto& segment = metadata->segments[i];
            uint64_t    end     = segment.mappedAddress + segment.memorySize;

            if (end > heapEnd)
            {
                heapEnd = end;
            }
        }

        process->breakBase    = align_up(heapEnd, PAGE_SIZE);
        process->programBreak = process->breakBase;
    }

    kfree(scratch);

    if (mappedPages != nullptr)
    {
        kfree(mappedPages);
    }

    return ARX_MEMORY_RETURN(true);

fail:

    for (uint64_t pageIndex = 0; pageIndex < mappedPageCount; ++pageIndex)
    {
        virtualMemoryManager->ReleaseAnonymousPage(process->addressSpace, mappedPages[pageIndex].virtualAddress);
    }

    for (uint64_t segmentIndex = 0; segmentIndex < metadata->segmentCount; ++segmentIndex)
    {
        metadata->segments[segmentIndex].mapped        = false;
        metadata->segments[segmentIndex].mappedAddress = 0;
    }

    metadata->executableLoaded = false;

    kfree(scratch);

    if (mappedPages != nullptr)
    {
        kfree(mappedPages);
    }

    return false;
}
