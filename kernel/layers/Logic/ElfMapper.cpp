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
    return a > (UINT64_MAX - b);
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
#if defined(__x86_64__)
    return machine == ELF_MACHINE_X86_64;
#elif defined(__aarch64__)
    return machine == ELF_MACHINE_AARCH64;
#else
    (void) machine;
    return false;
#endif
}

bool ElfMapper::ValidateUserspaceRange(uint64_t virtualAddress, uint64_t memorySize) const
{
    if (memorySize == 0)
    {
        return virtualAddress <= CANONICAL_USER_END;
    }

    if (AddWouldOverflow(virtualAddress, memorySize - 1))
    {
        return false;
    }

    const uint64_t inclusiveEnd = virtualAddress + (memorySize - 1);
    if (virtualAddress < CANONICAL_USER_BASE || inclusiveEnd > CANONICAL_USER_END)
    {
        return false;
    }

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
    if (programHeader.fileSize > programHeader.memorySize)
    {
        return false;
    }

    if (programHeader.fileSize > 0)
    {
        if (programHeader.offset > fileSize)
        {
            return false;
        }

        if (programHeader.fileSize > fileSize - programHeader.offset)
        {
            return false;
        }
    }

    if (!ValidateUserspaceRange(programHeader.virtualAddress, programHeader.memorySize))
    {
        return false;
    }

    if (programHeader.alignment != 0 && programHeader.alignment != 1)
    {
        if (!IsPowerOfTwo(programHeader.alignment))
        {
            return false;
        }

        if ((programHeader.virtualAddress % programHeader.alignment) != (programHeader.offset % programHeader.alignment))
        {
            return false;
        }
    }

    return true;
}

bool ElfMapper::ValidateHeader(const elf64_header_t& header, uint64_t fileSize) const
{
    if (header.ident[0] != ELF_MAGIC_0 || header.ident[1] != ELF_MAGIC_1 || header.ident[2] != ELF_MAGIC_2 || header.ident[3] != ELF_MAGIC_3)
    {
        return false;
    }

    if (header.ident[4] != ELF_CLASS_64)
    {
        return false;
    }

    if (header.ident[5] != ELF_DATA_LSB)
    {
        return false;
    }

    if (header.ident[6] != ELF_VERSION_CURRENT || header.version != ELF_VERSION_CURRENT)
    {
        return false;
    }

    if (!IsSupportedMachine(header.machine))
    {
        return false;
    }

    if (header.type != ELF_TYPE_EXEC && header.type != ELF_TYPE_DYN)
    {
        return false;
    }

    if (header.headerSize < sizeof(elf64_header_t))
    {
        return false;
    }

    if (header.programHeaderCount == 0 || header.programHeaderCount > ELF_MAX_PROGRAM_HEADERS)
    {
        return false;
    }

    if (header.programHeaderEntrySize != sizeof(elf64_program_header_t))
    {
        return false;
    }

    if (header.programHeaderOffset > fileSize)
    {
        return false;
    }

    if (AddWouldOverflow((uint64_t) header.programHeaderEntrySize, (uint64_t) header.programHeaderCount))
    {
        return false;
    }

    const uint64_t tableSize = (uint64_t) header.programHeaderEntrySize * (uint64_t) header.programHeaderCount;
    if (tableSize > fileSize - header.programHeaderOffset)
    {
        return false;
    }

    return true;
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
    if (file == nullptr || metadata == nullptr)
    {
        return false;
    }

    if (file->inode == nullptr)
    {
        return false;
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
        return false;
    }

    elf64_header_t header = {};
    if (!ReadExactAt(file, 0, &header, sizeof(header)))
    {
        return false;
    }

    if (!ValidateHeader(header, fileSize))
    {
        return false;
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
        return false;
    }

    return true;
}

bool ElfMapper::LoadExecutable(process_t* process, file_t* file, elf_metadata_t* metadata)
{
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

    if (metadata->type != ELF_TYPE_EXEC)
    {
        return false;
    }

    metadata->loadBias   = 0;
    metadata->entryPoint = metadata->originalEntryPoint;

    VirtualMemoryManager*  virtualMemoryManager  = ResourceLayerImportCaps->virtualMemoryManager;
    PhysicalMemoryManager* physicalMemoryManager = ResourceLayerImportCaps->physicalMemoryManager;

    struct mapped_page_state_t
    {
        uint64_t virtualAddress;
        void*    pagePointer;
        uint32_t combinedFlags;
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

            void* pageBuffer = physicalMemoryManager->Alloc(PAGE_SIZE);
            if (pageBuffer == nullptr)
            {
                goto fail;
            }

            memset(pageBuffer, 0, PAGE_SIZE);

            uint64_t pageFlags = 0;
            ARCH_PAGE_FLAGS_INIT(pageFlags);
            ARCH_PAGE_FLAG_SET_READ(pageFlags);
            ARCH_PAGE_FLAG_SET_WRITE(pageFlags);
            ARCH_PAGE_FLAG_SET_USER(pageFlags);

            const phys_addr_t pagePhysicalAddress = (phys_addr_t) hhdm_to_pa((uintptr_t) pageBuffer, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
            virtualMemoryManager->MapPage(pageAddress, pagePhysicalAddress, pageFlags, process->addressSpace);

            if (mappedPageCount >= maxPageCount)
            {
                goto fail;
            }

            mappedPages[mappedPageCount].virtualAddress = pageAddress;
            mappedPages[mappedPageCount].pagePointer    = pageBuffer;
            mappedPages[mappedPageCount].combinedFlags  = segment.flags;
            mappedPageCount++;
        }

        uint64_t sourceOffset = segment.fileOffset;
        uint64_t destination  = mappedAddress;
        uint64_t bytesToCopy  = segment.fileSize;

        while (bytesToCopy > 0)
        {
            const uint64_t pageOffset = destination & (PAGE_SIZE - 1);
            const uint64_t chunkSize  = MinU64(PAGE_SIZE - pageOffset, bytesToCopy);

            if (!ReadExactAt(file, sourceOffset, scratch, chunkSize))
            {
                goto fail;
            }

            const uint64_t pageAddress = align_down(destination, PAGE_SIZE);
            int64_t        pageIndex   = findMappedPageIndex(pageAddress);
            if (pageIndex < 0)
            {
                goto fail;
            }

            memcpy((uint8_t*) mappedPages[(uint64_t) pageIndex].pagePointer + pageOffset, scratch, (size_t) chunkSize);

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

        virtualMemoryManager->ProtectPage(mappedPages[pageIndex].virtualAddress, protectionFlags, process->addressSpace);
    }

    metadata->entryPoint       = metadata->loadBias + metadata->originalEntryPoint;
    metadata->executableLoaded = true;

    kfree(scratch);
    if (mappedPages != nullptr)
    {
        kfree(mappedPages);
    }

    return true;

fail:
    for (uint64_t pageIndex = 0; pageIndex < mappedPageCount; ++pageIndex)
    {
        virtualMemoryManager->UnmapPage(mappedPages[pageIndex].virtualAddress, process->addressSpace);
        if (mappedPages[pageIndex].pagePointer != nullptr)
        {
            physicalMemoryManager->Free(mappedPages[pageIndex].pagePointer);
        }
    }

    for (uint64_t segmentIndex = 0; segmentIndex < metadata->segmentCount; ++segmentIndex)
    {
        metadata->segments[segmentIndex].mapped = false;
    }

    metadata->executableLoaded = false;

    kfree(scratch);
    if (mappedPages != nullptr)
    {
        kfree(mappedPages);
    }

    return false;
}
