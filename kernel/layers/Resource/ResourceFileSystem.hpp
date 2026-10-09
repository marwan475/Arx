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

struct resource_directory_entry_t
{
    char                 name[256];
    uint64_t             inodeNumber;
    resource_node_type_t type;
};

struct ResourceLayerFileSystemCaps
{
    void* context;

    resource_fs_t* (*MountFilesystem)(ResourceLayerFileSystemCaps* caps, const char* type, void* source);
    resource_node_t* (*GetRootNode)(ResourceLayerFileSystemCaps* caps, resource_fs_t* filesystem);
    bool (*GetNodeInfo)(ResourceLayerFileSystemCaps* caps, resource_node_t* node, resource_node_info_t* info);
    resource_node_t* (*Lookup)(ResourceLayerFileSystemCaps* caps, resource_node_t* directory, const char* name);
    int64_t (*ReadDirectory)(ResourceLayerFileSystemCaps* caps, resource_node_t* directory, uint64_t* cursor, resource_directory_entry_t* entry);
    int64_t (*Read)(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, void* buffer, uint64_t size);
    int64_t (*Write)(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, const void* buffer, uint64_t size);
    int64_t (*Mkdir)(ResourceLayerFileSystemCaps* caps, const char* path, uint32_t mode);
    int64_t (*Unlink)(ResourceLayerFileSystemCaps* caps, const char* path, bool directory);
    int64_t (*Rename)(ResourceLayerFileSystemCaps* caps, const char* oldPath, const char* newPath);
    int64_t (*Link)(ResourceLayerFileSystemCaps* caps, const char* oldPath, const char* newPath, bool followSymlink);
    int64_t (*Mknod)(ResourceLayerFileSystemCaps* caps, const char* path, uint32_t mode, uint64_t device);
    int64_t (*Truncate)(ResourceLayerFileSystemCaps* caps, const char* path, uint64_t size);
};

class ResourceFileSystem
{
public:
    explicit ResourceFileSystem(InitRamFileSystemManager* initRamFileSystemManager);

    ResourceLayerFileSystemCaps* GetCaps();

    resource_fs_t*   MountFilesystem(const char* type, void* source);
    resource_node_t* GetRootNode(resource_fs_t* filesystem);
    bool             GetNodeInfo(resource_node_t* node, resource_node_info_t* info);
    resource_node_t* Lookup(resource_node_t* directory, const char* name);
    int64_t          ReadDirectory(resource_node_t* directory, uint64_t* cursor, resource_directory_entry_t* entry);
    int64_t          Read(resource_node_t* node, uint64_t offset, void* buffer, uint64_t size);
    int64_t          Write(resource_node_t* node, uint64_t offset, const void* buffer, uint64_t size);
    int64_t          Mkdir(const char* path, uint32_t mode);
    int64_t          Unlink(const char* path, bool directory);
    int64_t          Rename(const char* oldPath, const char* newPath);
    int64_t          Link(const char* oldPath, const char* newPath, bool followSymlink);
    int64_t          Mknod(const char* path, uint32_t mode, uint64_t device);
    int64_t          Truncate(const char* path, uint64_t size);

private:
    static ResourceFileSystem* FromCaps(ResourceLayerFileSystemCaps* caps);

    static resource_fs_t*   MountFilesystemThunk(ResourceLayerFileSystemCaps* caps, const char* type, void* source);
    static resource_node_t* GetRootNodeThunk(ResourceLayerFileSystemCaps* caps, resource_fs_t* filesystem);
    static bool             GetNodeInfoThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* node, resource_node_info_t* info);
    static resource_node_t* LookupThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* directory, const char* name);
    static int64_t          ReadDirectoryThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* directory, uint64_t* cursor,
                                               resource_directory_entry_t* entry);
    static int64_t          ReadThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, void* buffer, uint64_t size);
    static int64_t          WriteThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, const void* buffer, uint64_t size);
    static int64_t          MkdirThunk(ResourceLayerFileSystemCaps* caps, const char* path, uint32_t mode);
    static int64_t          UnlinkThunk(ResourceLayerFileSystemCaps* caps, const char* path, bool directory);
    static int64_t          RenameThunk(ResourceLayerFileSystemCaps* caps, const char* oldPath, const char* newPath);
    static int64_t          LinkThunk(ResourceLayerFileSystemCaps* caps, const char* oldPath, const char* newPath, bool followSymlink);
    static int64_t          MknodThunk(ResourceLayerFileSystemCaps* caps, const char* path, uint32_t mode, uint64_t device);
    static int64_t          TruncateThunk(ResourceLayerFileSystemCaps* caps, const char* path, uint64_t size);

    InitRamFileSystemManager*   InitRamManager;
    ResourceLayerFileSystemCaps Caps;
};
