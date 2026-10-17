#pragma once

#include "layers/Logic/VirtualFileSystem.hpp"

#include <klib/klib.h>

// A writable backend for socket namespace contracts. Unlinked inodes retain
// their identity until the VFS is destroyed, as required by the backend ABI.
struct SocketFileSystemFixture
{
    struct node_t
    {
        inode_t  inode;
        inode_t* alias;
        char     name[108];
        bool     present;
    };

    SocketFileSystemFixture()
    {
        memset(this, 0, sizeof(*this));
        static const filesystem_type_t type = {"socket-test", nullptr, nullptr, nullptr};
        operations.Lookup                   = Lookup;
        operations.Mknod                    = Mknod;
        operations.Unlink                   = Unlink;
        operations.Rename                   = Rename;
        operations.Link                     = Link;
        root.type                           = INODE_DIRECTORY;
        root.mode                           = 0777;
        root.metadataValid                  = true;
        root.inodeOps                       = &operations;
        root.privateData                    = this;
        root.filesystem                     = &filesystem;
        filesystem.type                     = &type;
        filesystem.rootInode                = &root;
        dentry.name                         = "/";
        dentry.inode                        = &root;
        mount.root                          = &dentry;
        mount.filesystem                    = &filesystem;
    }

    static inode_t* Lookup(inode_t* directory, const char* name)
    {
        auto* fixture = static_cast<SocketFileSystemFixture*>(directory->privateData);
        for (auto& node : fixture->nodes)
        {
            if (node.present && strcmp(node.name, name) == 0)
            {
                return node.alias != nullptr ? node.alias : &node.inode;
            }
        }
        return nullptr;
    }

    static int64_t Mknod(inode_t* directory, const char* name, const inode_creation_attributes_t* attributes, uint64_t)
    {
        auto* fixture = static_cast<SocketFileSystemFixture*>(directory->privateData);
        if (Lookup(directory, name) != nullptr)
        {
            return -17;
        }
        if ((attributes->mode & 0170000) != 0140000 || strlen(name) >= 108)
        {
            return -22;
        }
        for (auto& node : fixture->nodes)
        {
            if (node.inode.inodeNumber != 0)
            {
                continue;
            }
            node.inode.inodeNumber   = ++fixture->sequence;
            node.inode.type          = INODE_SOCKET;
            node.inode.mode          = attributes->mode & 07777;
            node.inode.uid           = attributes->uid;
            node.inode.gid           = attributes->gid;
            node.inode.metadataValid = true;
            node.inode.linkCount     = 1;
            node.inode.filesystem    = directory->filesystem;
            memcpy(node.name, name, strlen(name) + 1);
            node.present = true;
            return 0;
        }
        return -28;
    }

    static int64_t Unlink(inode_t* directory, const char* name, bool directoryEntry)
    {
        auto* fixture = static_cast<SocketFileSystemFixture*>(directory->privateData);
        if (directoryEntry)
        {
            return -20;
        }
        for (auto& node : fixture->nodes)
        {
            if (node.present && strcmp(node.name, name) == 0)
            {
                node.present = false;
                --(node.alias != nullptr ? node.alias : &node.inode)->linkCount;
                return 0;
            }
        }
        return -2;
    }

    static int64_t Link(inode_t* directory, const char* name, inode_t* inode)
    {
        auto* fixture = static_cast<SocketFileSystemFixture*>(directory->privateData);
        if (Lookup(directory, name) != nullptr || strlen(name) >= 108)
        {
            return -17;
        }
        for (auto& node : fixture->nodes)
        {
            if (node.inode.inodeNumber == 0)
            {
                node.inode.inodeNumber = ++fixture->sequence;
                node.alias             = inode;
                node.present           = true;
                memcpy(node.name, name, strlen(name) + 1);
                ++inode->linkCount;
                return 0;
            }
        }
        return -28;
    }

    static int64_t Rename(inode_t* directory, const char* name, inode_t* destination, const char* newName)
    {
        if (directory != destination || strlen(newName) >= 108)
        {
            return -22;
        }
        auto* fixture = static_cast<SocketFileSystemFixture*>(directory->privateData);
        for (auto& node : fixture->nodes)
        {
            if (node.present && strcmp(node.name, name) == 0)
            {
                memcpy(node.name, newName, strlen(newName) + 1);
                return 0;
            }
        }
        return -2;
    }

    inode_operations_t operations;
    inode_t            root;
    filesystem_t       filesystem;
    dentry_t           dentry;
    mount_t            mount;
    uint64_t           sequence;
    node_t             nodes[16];
};
