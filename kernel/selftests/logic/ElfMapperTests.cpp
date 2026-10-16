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
} // namespace

void test_ElfMapper(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto*        m = logic ? logic->elfMapper : nullptr;
    ManagerSuite suite("logic/ElfMapper", m != nullptr);
    suite.Run("logic/ElfMapper/input_validation",
              [&](TestCase& t)
              {
                  elf_metadata_t metadata = {};
                  file_t         file     = {};
                  t.Check(!m->ReadElf(nullptr, &metadata), "missing file rejected");
                  t.Check(!m->ReadElf(&file, nullptr), "missing metadata rejected");
                  t.Check(!m->ReadElf(&file, &metadata), "missing inode rejected");
              });

    suite.Run("logic/ElfMapper/truncated_header",
              [&](TestCase& t)
              {
                  elf_metadata_t metadata = {};
                  inode_t        inode    = {};
                  inode.size              = 4;
                  file_t file             = {};
                  file.inode              = &inode;
                  t.Check(!m->ReadElf(&file, &metadata), "truncated ELF header rejected");
                  t.Check(metadata.segments == nullptr && !metadata.executableLoaded, "rejection allocates no metadata");
              });

    suite.Run("logic/ElfMapper/parse_load_segment",
              [&](TestCase& t)
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

    suite.Run("logic/ElfMapper/malformed_segment",
              [&](TestCase& t)
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
    suite.Run("logic/ElfMapper/library_contract",
              [&](TestCase& t)
              {
                  TestFixture fixture(resources);

                  if (!fixture.Valid(t))
                  {
                      return;
                  }

                  MemoryFileFixture backend(logic->virtualFileSystem);
                  initialize_elf(backend);
                  elf_metadata_t metadata = {};
                  t.Check(m->ReadElf(&backend.file, &metadata), "parse executable fixture");
                  t.Check(!m->LoadLibrary(fixture.Process, &backend.file, &metadata), "ET_EXEC is not a library");
                  metadata.type = 3;
                  metadata.originalEntryPoint -= metadata.segments[0].virtualAddress;
                  metadata.segments[0].virtualAddress = 0;
                  fixture.Process->breakBase          = 0x200000;
                  fixture.Process->programBreak       = 0x201000;
                  t.Check(!m->LoadLibrary(fixture.Process, &backend.file, &metadata, 0x40000001), "explicit misaligned bias rejected");
                  t.Check(m->LoadLibrary(fixture.Process, &backend.file, &metadata), "automatic library placement");
                  uint64_t first = metadata.loadBias;
                  t.Check(!m->LoadLibrary(fixture.Process, &backend.file, &metadata, first), "explicit occupied bias rejected");
                  t.Check(m->LoadLibrary(fixture.Process, &backend.file, &metadata), "second automatic mapping finds another range");
                  t.Check(metadata.loadBias != first, "automatic placements do not overlap");
                  t.Equal(fixture.Process->breakBase, 0x200000, "library preserves heap base");
                  t.Equal(fixture.Process->programBreak, 0x201000, "library preserves current break");
                  t.Check(!m->LoadLibrary(fixture.Process, &backend.file, &metadata, UINT64_MAX - PAGE_SIZE + 1), "overflowing image rejected");
        t.Check(m->LoadLibrary(fixture.Process, &backend.file, &metadata, CANONICAL_USER_END + 1 - PAGE_SIZE), "last userspace page is a valid exact placement");
                  kfree(metadata.segments);
              });

    suite.Run("logic/ElfMapper/library_rollback",
              [&](TestCase& t)
              {
                  TestFixture fixture(resources);

                  if (!fixture.Valid(t))
                  {
                      return;
                  }

                  MemoryFileFixture backend(logic->virtualFileSystem);
                  initialize_elf(backend);
                  elf_metadata_t metadata = {};
                  t.Check(m->ReadElf(&backend.file, &metadata), "parse rollback fixture");
                  metadata.type                       = 3;
                  metadata.segments[0].virtualAddress = 0;
                  backend.operations.Read             = [](file_t*, void*, uint64_t) -> int64_t { return -5; };
                  constexpr uint64_t bias             = 0x50000000;
                  t.Check(!m->LoadLibrary(fixture.Process, &backend.file, &metadata, bias), "read failure after allocation rejects load");
                  t.Check(resources->virtualMemoryManager->VirtToPhys(bias, fixture.Space) == 0, "failed load removes page table mapping");
                  t.Check(!metadata.executableLoaded && !metadata.segments[0].mapped && metadata.segments[0].mappedAddress == 0, "failed load clears mapping metadata");
                  backend.operations.Read = MemoryFileFixture::Read;
                  t.Check(m->LoadLibrary(fixture.Process, &backend.file, &metadata, bias), "failed load releases virtual reservation");
                  kfree(metadata.segments);
              });
}
