#include "layers/Resource/ResourceFileSystem.hpp"

#include "layers/Resource/InitRamFileSystemManager.hpp"

extern "C"
{
#include <klib/klib.h>
}

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

static uint64_t resource_hash_path(const char* path, bool isDirectory)
{
    uint64_t hash = 1469598103934665603ULL;

    if (path != nullptr)
    {
        for (const uint8_t* p = (const uint8_t*) path; *p != 0; ++p)
        {
            hash ^= *p;
            hash *= 1099511628211ULL;
        }
    }

    hash ^= isDirectory ? 0xD1ULL : 0xF1ULL;
    hash *= 1099511628211ULL;

    return hash;
}

static bool has_component_separator(const char* name)
{
    if (name == nullptr)
    {
        return true;
    }

    for (const char* p = name; *p != 0; ++p)
    {
        if (*p == '/')
        {
            return true;
        }
    }

    return false;
}

static bool path_starts_with_component(const char* path, const char* prefix)
{
    if (path == nullptr || prefix == nullptr)
    {
        return false;
    }

    const size_t prefixLen = strlen(prefix);
    if (prefixLen == 0)
    {
        return true;
    }

    if (memcmp(path, prefix, prefixLen) != 0)
    {
        return false;
    }

    return path[prefixLen] == '\0' || path[prefixLen] == '/';
}

static bool is_duplicate_name(const char* const* names, size_t count, const char* name, size_t nameLen)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (names[i] == nullptr)
        {
            continue;
        }

        if (strlen(names[i]) == nameLen && memcmp(names[i], name, nameLen) == 0)
        {
            return true;
        }
    }

    return false;
}

static resource_node_t* make_node(resource_fs_t* fs, const char* path, bool isDirectory, uint64_t size)
{
    if (fs == nullptr)
    {
        return nullptr;
    }

    resource_node_t* node = (resource_node_t*) kmalloc(sizeof(resource_node_t));
    if (node == nullptr)
    {
        return nullptr;
    }

    memset(node, 0, sizeof(resource_node_t));
    node->fs          = fs;
    node->isDirectory = isDirectory;
    node->size        = size;

    const char* safePath = path != nullptr ? path : "";
    node->path           = kstrdup(safePath);
    if (node->path == nullptr)
    {
        kfree(node);
        return nullptr;
    }

    return node;
}

ResourceFileSystem::ResourceFileSystem(InitRamFileSystemManager* initRamFileSystemManager) : InitRamManager(initRamFileSystemManager)
{
    Caps.context         = this;
    Caps.MountFilesystem = MountFilesystemThunk;
    Caps.GetRootNode     = GetRootNodeThunk;
    Caps.GetNodeInfo     = GetNodeInfoThunk;
    Caps.Lookup          = LookupThunk;
    Caps.ReadDirectory   = ReadDirectoryThunk;
    Caps.Read            = ReadThunk;
    Caps.Write           = WriteThunk;
}

ResourceLayerFileSystemCaps* ResourceFileSystem::GetCaps()
{
    return &Caps;
}

ResourceFileSystem* ResourceFileSystem::FromCaps(ResourceLayerFileSystemCaps* caps)
{
    if (caps == nullptr)
    {
        return nullptr;
    }

    return (ResourceFileSystem*) caps->context;
}

resource_fs_t* ResourceFileSystem::MountFilesystemThunk(ResourceLayerFileSystemCaps* caps, const char* type, void* source)
{
    ResourceFileSystem* self = FromCaps(caps);
    return self != nullptr ? self->MountFilesystem(type, source) : nullptr;
}

resource_node_t* ResourceFileSystem::GetRootNodeThunk(ResourceLayerFileSystemCaps* caps, resource_fs_t* filesystem)
{
    ResourceFileSystem* self = FromCaps(caps);
    return self != nullptr ? self->GetRootNode(filesystem) : nullptr;
}

bool ResourceFileSystem::GetNodeInfoThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* node, resource_node_info_t* info)
{
    ResourceFileSystem* self = FromCaps(caps);
    return self != nullptr ? self->GetNodeInfo(node, info) : false;
}

resource_node_t* ResourceFileSystem::LookupThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* directory, const char* name)
{
    ResourceFileSystem* self = FromCaps(caps);
    return self != nullptr ? self->Lookup(directory, name) : nullptr;
}

int64_t ResourceFileSystem::ReadDirectoryThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* directory, uint64_t* cursor,
                                               resource_directory_entry_t* entry)
{
    ResourceFileSystem* self = FromCaps(caps);
    return self != nullptr ? self->ReadDirectory(directory, cursor, entry) : -1;
}

int64_t ResourceFileSystem::ReadThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, void* buffer, uint64_t size)
{
    ResourceFileSystem* self = FromCaps(caps);
    return self != nullptr ? self->Read(node, offset, buffer, size) : -1;
}

int64_t ResourceFileSystem::WriteThunk(ResourceLayerFileSystemCaps* caps, resource_node_t* node, uint64_t offset, const void* buffer, uint64_t size)
{
    ResourceFileSystem* self = FromCaps(caps);
    return self != nullptr ? self->Write(node, offset, buffer, size) : -1;
}

resource_fs_t* ResourceFileSystem::MountFilesystem(const char* type, void* source)
{
    if (type == nullptr || strcmp(type, "cpio") != 0)
    {
        return nullptr;
    }

    InitRamFileSystemManager* manager = source != nullptr ? (InitRamFileSystemManager*) source : InitRamManager;
    if (manager == nullptr)
    {
        return nullptr;
    }

    resource_fs_t* fs = (resource_fs_t*) kmalloc(sizeof(resource_fs_t));
    if (fs == nullptr)
    {
        return nullptr;
    }

    fs->initRamManager = manager;
    return fs;
}

resource_node_t* ResourceFileSystem::GetRootNode(resource_fs_t* filesystem)
{
    return make_node(filesystem, "", true, 0);
}

bool ResourceFileSystem::GetNodeInfo(resource_node_t* node, resource_node_info_t* info)
{
    if (node == nullptr || info == nullptr)
    {
        return false;
    }

    info->inodeNumber = resource_hash_path(node->path, node->isDirectory);
    info->size        = node->size;
    info->type        = node->isDirectory ? RESOURCE_NODE_DIRECTORY : RESOURCE_NODE_REGULAR;
    return true;
}

resource_node_t* ResourceFileSystem::Lookup(resource_node_t* directory, const char* name)
{
    if (directory == nullptr || directory->fs == nullptr || directory->fs->initRamManager == nullptr || name == nullptr || name[0] == '\0')
    {
        return nullptr;
    }

    if (!directory->isDirectory || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || has_component_separator(name))
    {
        return nullptr;
    }

    const char*  base      = directory->path != nullptr ? directory->path : "";
    const size_t baseLen   = strlen(base);
    const size_t nameLen   = strlen(name);
    const bool   hasPrefix = baseLen > 0;
    const size_t fullLen   = hasPrefix ? (baseLen + 1 + nameLen) : nameLen;

    char* fullPath = (char*) kmalloc(fullLen + 1);
    if (fullPath == nullptr)
    {
        return nullptr;
    }

    if (hasPrefix)
    {
        memcpy(fullPath, base, baseLen);
        fullPath[baseLen] = '/';
        memcpy(fullPath + baseLen + 1, name, nameLen);
    }
    else
    {
        memcpy(fullPath, name, nameLen);
    }
    fullPath[fullLen] = '\0';

    initramfs_archive_t* file = directory->fs->initRamManager->find(fullPath);
    if (file != nullptr)
    {
        resource_node_t* node = make_node(directory->fs, fullPath, false, file->size);
        kfree(fullPath);
        return node;
    }

    const size_t archiveCount = directory->fs->initRamManager->GetArchiveCount();
    bool         directoryFound = false;
    for (size_t i = 0; i < archiveCount; ++i)
    {
        const initramfs_archive_t* archive = directory->fs->initRamManager->GetArchiveAt(i);
        if (archive == nullptr || archive->path == nullptr)
        {
            continue;
        }

        if (!path_starts_with_component(archive->path, fullPath))
        {
            continue;
        }

        const size_t fullPathLen = strlen(fullPath);
        if (strlen(archive->path) > fullPathLen && archive->path[fullPathLen] == '/')
        {
            directoryFound = true;
            break;
        }
    }

    if (directoryFound)
    {
        resource_node_t* node = make_node(directory->fs, fullPath, true, 0);
        kfree(fullPath);
        return node;
    }

    kfree(fullPath);
    return nullptr;
}

int64_t ResourceFileSystem::ReadDirectory(resource_node_t* directory, uint64_t* cursor, resource_directory_entry_t* entry)
{
    if (directory == nullptr || directory->fs == nullptr || directory->fs->initRamManager == nullptr || cursor == nullptr || entry == nullptr)
    {
        return -1;
    }

    if (!directory->isDirectory)
    {
        return -1;
    }

    const char* basePath = directory->path != nullptr ? directory->path : "";
    const size_t baseLen = strlen(basePath);
    const size_t archiveCount = directory->fs->initRamManager->GetArchiveCount();
    const uint64_t targetIndex = *cursor;
    uint64_t currentIndex = 0;

    // Track discovered names to avoid duplicate directory entries from multiple descendants.
    const char* discoveredNames[1024] = {};
    size_t discoveredCount = 0;

    for (size_t i = 0; i < archiveCount; ++i)
    {
        const initramfs_archive_t* archive = directory->fs->initRamManager->GetArchiveAt(i);
        if (archive == nullptr || archive->path == nullptr)
        {
            continue;
        }

        const char* path = archive->path;
        if (baseLen > 0)
        {
            if (memcmp(path, basePath, baseLen) != 0 || path[baseLen] != '/')
            {
                continue;
            }
            path += baseLen + 1;
        }

        if (*path == '\0')
        {
            continue;
        }

        const char* separator = nullptr;
        for (const char* p = path; *p != '\0'; ++p)
        {
            if (*p == '/')
            {
                separator = p;
                break;
            }
        }
        const size_t nameLen = separator != nullptr ? (size_t) (separator - path) : strlen(path);
        if (nameLen == 0 || nameLen >= sizeof(entry->name))
        {
            continue;
        }

        if (is_duplicate_name(discoveredNames, discoveredCount, path, nameLen))
        {
            continue;
        }

        if (discoveredCount < (sizeof(discoveredNames) / sizeof(discoveredNames[0])))
        {
            discoveredNames[discoveredCount++] = path;
        }

        if (currentIndex == targetIndex)
        {
            memset(entry, 0, sizeof(*entry));
            memcpy(entry->name, path, nameLen);
            entry->name[nameLen] = '\0';
            entry->type          = separator != nullptr ? RESOURCE_NODE_DIRECTORY : RESOURCE_NODE_REGULAR;

            char fullPath[512] = {};
            if (baseLen > 0)
            {
                memcpy(fullPath, basePath, baseLen);
                fullPath[baseLen] = '/';
                memcpy(fullPath + baseLen + 1, entry->name, nameLen + 1);
            }
            else
            {
                memcpy(fullPath, entry->name, nameLen + 1);
            }

            entry->inodeNumber = resource_hash_path(fullPath, entry->type == RESOURCE_NODE_DIRECTORY);
            *cursor += 1;
            return 1;
        }

        currentIndex += 1;
    }

    return 0;
}

int64_t ResourceFileSystem::Read(resource_node_t* node, uint64_t offset, void* buffer, uint64_t size)
{
    if (node == nullptr || node->fs == nullptr || node->fs->initRamManager == nullptr || buffer == nullptr)
    {
        return -1;
    }

    if (node->isDirectory)
    {
        return -1;
    }

    initramfs_archive_t* file = node->fs->initRamManager->find(node->path);
    if (file == nullptr)
    {
        return -1;
    }

    if (offset >= file->size)
    {
        return 0;
    }

    uint64_t remaining = file->size - offset;
    uint64_t toRead    = size < remaining ? size : remaining;
    memcpy(buffer, file->data + offset, toRead);
    return (int64_t) toRead;
}

int64_t ResourceFileSystem::Write(resource_node_t* node, uint64_t offset, const void* buffer, uint64_t size)
{
    if (node == nullptr || node->fs == nullptr || node->fs->initRamManager == nullptr || buffer == nullptr)
    {
        return -1;
    }

    if (node->isDirectory)
    {
        return -1;
    }

    initramfs_archive_t* file = node->fs->initRamManager->find(node->path);
    if (file == nullptr)
    {
        return -1;
    }

    if (offset >= file->size)
    {
        return 0;
    }

    uint64_t remaining = file->size - offset;
    uint64_t toWrite   = size < remaining ? size : remaining;
    memcpy(file->data + offset, buffer, toWrite);
    return (int64_t) toWrite;
}
