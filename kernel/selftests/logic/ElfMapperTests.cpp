#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>
#include <selftests/MemoryFileFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/ElfMapper.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"

namespace
{
struct __attribute__((packed)) test_elf_header_t
{
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t programOffset;
    uint64_t sectionOffset;
    uint32_t flags;
    uint16_t headerSize;
    uint16_t programSize;
    uint16_t programCount;
    uint16_t sectionSize;
    uint16_t sectionCount;
    uint16_t stringIndex;
};

struct __attribute__((packed)) test_elf_segment_t
{
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t address;
    uint64_t physical;
    uint64_t fileSize;
    uint64_t memorySize;
    uint64_t alignment;
};

static_assert(sizeof(test_elf_header_t) == 64 && sizeof(test_elf_segment_t) == 56, "ELF64 fixture wire layout");

void initialize_elf(MemoryFileFixture& backend)
{
    test_elf_header_t header = {};
    memcpy(header.ident, "\177ELF\002\001\001", 7);
    header.type = 2;
#if defined(__x86_64__)
    header.machine = 62;
#else
    header.machine = 183;
#endif
    header.version       = 1;
    header.entry         = 0x400080;
    header.programOffset = sizeof(header);
    header.headerSize    = sizeof(header);
    header.programSize   = sizeof(test_elf_segment_t);
    header.programCount  = 1;

    test_elf_segment_t segment = {1, 5, 0, 0x400000, 0, 128, PAGE_SIZE, PAGE_SIZE};
    memcpy(backend.bytes, &header, sizeof(header));
    memcpy(backend.bytes + sizeof(header), &segment, sizeof(segment));
    backend.inode.size = 128;
}
}

void test_ElfMapper(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->elfMapper : nullptr;
    ManagerSuite suite("logic/ElfMapper", m != nullptr);
    suite.Run("logic/ElfMapper/input_validation", [&](TestCase& t)
    {
        elf_metadata_t metadata = {};
        file_t         file     = {};
        t.Check(!m->ReadElf(nullptr, &metadata), "missing file rejected");
        t.Check(!m->ReadElf(&file, nullptr), "missing metadata rejected");
        t.Check(!m->ReadElf(&file, &metadata), "missing inode rejected");
    });

    suite.Run("logic/ElfMapper/truncated_header", [&](TestCase& t)
    {
        elf_metadata_t metadata = {};
        inode_t        inode    = {};
        inode.size              = 4;
        file_t file             = {};
        file.inode              = &inode;
        t.Check(!m->ReadElf(&file, &metadata), "truncated ELF header rejected");
        t.Check(metadata.segments == nullptr && !metadata.executableLoaded, "rejection allocates no metadata");
    });

    suite.Run("logic/ElfMapper/parse_load_segment", [&](TestCase& t)
    {
        if (!logic->virtualFileSystem)
        {
            t.Block("VFS fixture dependency unavailable");
            return;
        }

        MemoryFileFixture backend(logic->virtualFileSystem);
        initialize_elf(backend);
        elf_metadata_t metadata = {};

        if (t.Check(m->ReadElf(&backend.file, &metadata), "parse valid ELF64 executable"))
        {
            t.Equal(metadata.entryPoint, 0x400080, "entry point decoded");
            t.Equal(metadata.segmentCount, 1, "one load segment");

            if (t.Check(metadata.segments != nullptr, "segment metadata allocated"))
            {
                t.Equal(metadata.segments[0].memorySize, PAGE_SIZE, "memory size exceeds file bytes for BSS");
            }

            t.Equal(backend.file.offset, 0, "metadata parsing preserves file position");
        }

        kfree(metadata.segments);
    });

    suite.Run("logic/ElfMapper/malformed_segment", [&](TestCase& t)
    {
        if (!logic->virtualFileSystem)
        {
            t.Block("VFS fixture dependency unavailable");
            return;
        }

        MemoryFileFixture backend(logic->virtualFileSystem);
        initialize_elf(backend);
        test_elf_segment_t segment = {1, 5, 0, 0x400000, 0, 129, 128, PAGE_SIZE};
        memcpy(backend.bytes + sizeof(test_elf_header_t), &segment, sizeof(segment));
        elf_metadata_t metadata = {};

        t.Check(!m->ReadElf(&backend.file, &metadata), "segment file bytes cannot exceed memory size");
        kfree(metadata.segments);
    });
}
