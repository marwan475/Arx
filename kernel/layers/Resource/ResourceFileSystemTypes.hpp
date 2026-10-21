#pragma once
#include <stdint.h>

// Retained backend layouts shared with matching native debug readers.
class InitRamFileSystemManager;

struct resource_fs_t
{
    InitRamFileSystemManager* initRamManager;
};

struct resource_node_t
{
    resource_fs_t* fs;
    char*          path;
    bool           isDirectory;
    uint64_t       size;
};
