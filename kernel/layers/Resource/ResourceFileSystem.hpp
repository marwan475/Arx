#pragma once

#include <stdint.h>

class InitRamFileSystemManager;

struct resource_fs_t;
struct resource_node_t;

enum resource_node_type_t
{
    RESOURCE_NODE_REGULAR,
    RESOURCE_NODE_DIRECTORY,
    RESOURCE_NODE_SYMLINK,
    RESOURCE_NODE_CHAR_DEVICE,
    RESOURCE_NODE_BLOCK_DEVICE,
};

struct resource_node_info_t
{
    uint64_t             inodeNumber;
    uint64_t             size;
    resource_node_type_t type;
};

struct ResourceLayerFileSystemCaps
{
    void* context;

    resource_fs_t* (*MountFilesystem)(ResourceLayerFileSystemCaps* caps, const char* type, void* source);
    resource_node_t* (*GetRootNode)(ResourceLayerFileSystemCaps* caps, resource_fs_t* filesystem);
    bool (*GetNodeInfo)(ResourceLayerFileSystemCaps* caps, resource_node_t* node, resource_node_info_t* info);
    resource_node_t* (*Lookup)(ResourceLayerFileSystemCaps* caps, resource_node_t* directory, const char* name);
    int64_t (*Read)(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, void* buffer, uint64_t size);
    int64_t (*Write)(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, const void* buffer, uint64_t size);
};

class ResourceFileSystem
{
public:
    explicit ResourceFileSystem(InitRamFileSystemManager* initRamFileSystemManager);

    ResourceLayerFileSystemCaps* GetCaps();

    resource_fs_t* MountFilesystem(const char* type, void* source);
    resource_node_t* GetRootNode(resource_fs_t* filesystem);
    bool GetNodeInfo(resource_node_t* node, resource_node_info_t* info);
    resource_node_t* Lookup(resource_node_t* directory, const char* name);
    int64_t Read(resource_node_t* node, uint64_t offset, void* buffer, uint64_t size);
    int64_t Write(resource_node_t* node, uint64_t offset, const void* buffer, uint64_t size);

private:
    static ResourceFileSystem* FromCaps(ResourceLayerFileSystemCaps* caps);

    static resource_fs_t* MountFilesystemThunk(ResourceLayerFileSystemCaps* caps, const char* type, void* source);
    static resource_node_t* GetRootNodeThunk(ResourceLayerFileSystemCaps* caps, resource_fs_t* filesystem);
    static bool GetNodeInfoThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* node, resource_node_info_t* info);
    static resource_node_t* LookupThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* directory, const char* name);
    static int64_t ReadThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, void* buffer, uint64_t size);
    static int64_t WriteThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, const void* buffer, uint64_t size);

    InitRamFileSystemManager* InitRamManager;
    ResourceLayerFileSystemCaps Caps;
};
