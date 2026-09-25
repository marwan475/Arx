#include "layers/Resource/InitRamFileSystemManager.hpp"

extern "C"
{
#include <klib/klib.h>
}

namespace
{
constexpr size_t CPIO_HEADER_SIZE     = 110;
constexpr size_t CPIO_MAGIC_SIZE      = 6;
constexpr size_t CPIO_FILESIZE_OFFSET = 54;
constexpr size_t CPIO_NAMESIZE_OFFSET = 94;

constexpr char CPIO_NEWC_MAGIC[] = "070701";
constexpr char CPIO_TRAILER[]    = "TRAILER!!!";

size_t normalizePathStart(const uint8_t* name, size_t nameLen)
{
    size_t start = 0;

    while (start + 1 < nameLen && name[start] == '.' && name[start + 1] == '/')
    {
        start += 2;
    }

    while (start < nameLen && name[start] == '/')
    {
        start++;
    }

    return start;
}
}

InitRamFileSystemManager::InitRamFileSystemManager(uint64_t initramfsSize, uintptr_t initramfsAddress)
    : ArchiveCount(0)
{
    for (size_t i = 0; i < MAX_ARCHIVES; i++)
    {
        Archives[i].path      = nullptr;
        Archives[i].size      = 0;
        Archives[i].data      = nullptr;
    }

    loadArchivesFromCpio(initramfsSize, initramfsAddress);
}

void InitRamFileSystemManager::reset()
{
    for (size_t i = 0; i < MAX_ARCHIVES; i++)
    {
        if (Archives[i].path != nullptr)
        {
            kfree(Archives[i].path);
            Archives[i].path = nullptr;
        }

        Archives[i].size      = 0;
        Archives[i].data      = nullptr;
    }

    ArchiveCount = 0;
}

bool InitRamFileSystemManager::parseHexField(const uint8_t* field, size_t len, uint64_t* valueOut)
{
    uint64_t value = 0;

    if (field == nullptr || valueOut == nullptr)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        uint8_t c = field[i];
        value <<= 4;

        if (c >= '0' && c <= '9')
        {
            value |= (uint64_t) (c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            value |= (uint64_t) (c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            value |= (uint64_t) (c - 'A' + 10);
        }
        else
        {
            return false;
        }
    }

    *valueOut = value;
    return true;
}

bool InitRamFileSystemManager::stringEquals(const char* lhs, const char* rhs)
{
    size_t lhsLen;
    size_t rhsLen;

    if (lhs == nullptr || rhs == nullptr)
    {
        return false;
    }

    lhsLen = strlen(lhs);
    rhsLen = strlen(rhs);

    if (lhsLen != rhsLen)
    {
        return false;
    }

    return memcmp(lhs, rhs, lhsLen) == 0;
}

bool InitRamFileSystemManager::loadArchivesFromCpio(uint64_t initramfsSize, uintptr_t initramfsAddress)
{
    const uint8_t* base = (const uint8_t*) initramfsAddress;
    uint64_t       offset;

    if (initramfsAddress == 0 || initramfsSize < CPIO_HEADER_SIZE)
    {
        return false;
    }

    reset();

    offset = 0;
    while (offset + CPIO_HEADER_SIZE <= initramfsSize)
    {
        const uint8_t* header;
        uint64_t       fileSize;
        uint64_t       nameSize;
        uint64_t       dataOffset;
        uint64_t       nextOffset;
        uint64_t       nameEndOffset;
        const uint8_t* namePtr;
        size_t         nameLen;
        size_t         normalizedStart;
        size_t         normalizedLen;

        header = base + offset;

        if (memcmp(header, CPIO_NEWC_MAGIC, CPIO_MAGIC_SIZE) != 0)
        {
            return false;
        }

        if (!parseHexField(header + CPIO_FILESIZE_OFFSET, 8, &fileSize))
        {
            return false;
        }

        if (!parseHexField(header + CPIO_NAMESIZE_OFFSET, 8, &nameSize))
        {
            return false;
        }

        if (nameSize == 0)
        {
            return false;
        }

        nameEndOffset = offset + CPIO_HEADER_SIZE + nameSize;
        if (nameEndOffset > initramfsSize)
        {
            return false;
        }

        namePtr = base + offset + CPIO_HEADER_SIZE;
        nameLen = (size_t) (nameSize - 1);

        dataOffset = align_up(nameEndOffset, 4);
        if (dataOffset > initramfsSize || fileSize > (initramfsSize - dataOffset))
        {
            return false;
        }

        nextOffset = align_up(dataOffset + fileSize, 4);
        if (nextOffset > initramfsSize)
        {
            return false;
        }

        if (nameLen == (sizeof(CPIO_TRAILER) - 1) && memcmp(namePtr, CPIO_TRAILER, sizeof(CPIO_TRAILER) - 1) == 0)
        {
            return true;
        }

        normalizedStart = normalizePathStart(namePtr, nameLen);
        normalizedLen   = nameLen - normalizedStart;

        if (normalizedLen > 0)
        {
            initramfs_archive_t* archive;
            char*                path;

            if (ArchiveCount >= MAX_ARCHIVES)
            {
                return false;
            }

            archive = &Archives[ArchiveCount];

            path = (char*) kmalloc(normalizedLen + 1);
            if (path == nullptr)
            {
                return false;
            }

            memcpy(path, namePtr + normalizedStart, normalizedLen);
            path[normalizedLen] = '\0';

            archive->path = path;
            archive->size = fileSize;
            archive->data = (uint8_t*) (base + dataOffset);
            ArchiveCount++;
        }

        offset = nextOffset;
    }

    return false;
}

initramfs_archive_t* InitRamFileSystemManager::find(const char* path)
{
    for (size_t i = 0; i < ArchiveCount; i++)
    {
        initramfs_archive_t* iter = &Archives[i];
        if (iter->path != nullptr && stringEquals(iter->path, path))
        {
            return iter;
        }
    }

    return nullptr;
}

size_t InitRamFileSystemManager::GetArchiveCount() const
{
    return ArchiveCount;
}
