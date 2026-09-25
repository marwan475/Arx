#pragma once

#include <stddef.h>
#include <stdint.h>

struct initramfs_archive_t
{
    char*    path;
    uint64_t size;
    uint8_t* data;
};

class InitRamFileSystemManager
{
public:
    static constexpr size_t MAX_ARCHIVES = 256;

    InitRamFileSystemManager(uint64_t initramfsSize, uintptr_t initramfsAddress);

    initramfs_archive_t* find(const char* path);

    size_t GetArchiveCount() const;

private:
    initramfs_archive_t Archives[MAX_ARCHIVES];
    size_t              ArchiveCount;

    void reset();
    bool loadArchivesFromCpio(uint64_t initramfsSize, uintptr_t initramfsAddress);

    static bool stringEquals(const char* lhs, const char* rhs);
    static bool parseHexField(const uint8_t* field, size_t len, uint64_t* valueOut);
};
