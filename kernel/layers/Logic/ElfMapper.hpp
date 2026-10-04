#pragma once

#include <stdint.h>

struct ResourceLayerCaps;
struct file_t;
struct process_t;

struct elf_segment_metadata_t
{
    uint64_t virtualAddress;
    uint64_t fileOffset;

    uint64_t fileSize;
    uint64_t memorySize;

    uint64_t alignment;
    uint32_t flags;

    uint64_t mappedAddress;
    bool     mapped;
};

struct elf_metadata_t
{
    uint16_t type;
    uint16_t machine;

    uint64_t originalEntryPoint;
    uint64_t entryPoint;

    uint64_t loadBias;

    uint64_t programHeaderOffset;
    uint16_t programHeaderEntrySize;
    uint16_t programHeaderCount;

    elf_segment_metadata_t* segments;
    uint64_t                segmentCount;

    bool executableLoaded;
};

class ElfMapper
{
public:
    explicit ElfMapper(ResourceLayerCaps* resourceLayerCaps);
    ~ElfMapper() = default;

    bool ReadElf(file_t* file, elf_metadata_t* metadata);
    bool LoadExecutable(process_t* process, file_t* file, elf_metadata_t* metadata);

private:
    struct elf64_header_t;
    struct elf64_program_header_t;

    bool ReadExactAt(file_t* file, uint64_t offset, void* buffer, uint64_t size) const;
    bool ValidateHeader(const elf64_header_t& header, uint64_t fileSize) const;
    bool ParseProgramHeaders(file_t* file, const elf64_header_t& header, uint64_t fileSize, elf_metadata_t* metadata) const;
    bool ValidateLoadSegment(const elf64_program_header_t& programHeader, uint64_t fileSize) const;
    bool ValidateUserspaceRange(uint64_t virtualAddress, uint64_t memorySize) const;
    bool IsSupportedMachine(uint16_t machine) const;

    static bool     AddWouldOverflow(uint64_t a, uint64_t b);
    static uint64_t MinU64(uint64_t a, uint64_t b);
    static bool     IsPowerOfTwo(uint64_t value);

    ResourceLayerCaps* ResourceLayerImportCaps;
};
