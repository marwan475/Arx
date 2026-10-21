#pragma once
#include "VirtualFileSystem.hpp"

// Internal retained layouts; callbacks remain opaque to inspection.
struct resource_fs_t;
struct resource_node_t;
struct ResourceLayerFileSystemCaps;

struct dentry_key_t
{
    const dentry_t* parent;
    const char*     name;
};

struct vfs_resource_filesystem_private_t
{
    resource_fs_t*               backend;
    ResourceLayerFileSystemCaps* caps;
};

struct vfs_resource_inode_private_t
{
    resource_node_t*             backendNode;
    ResourceLayerFileSystemCaps* caps;
};

struct vfs_symlink_private_t
{
    char* target;
};

struct vfs_overlay_dentry_entry_t
{
    dentry_t*                   dentry;
    vfs_overlay_dentry_entry_t* next;
};
