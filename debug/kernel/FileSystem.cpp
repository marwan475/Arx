#include "DebugManager.hpp"
#include "stats/FileSystemDebugStats.hpp"
#include <layers/Resource/InitRamFileSystemManager.hpp>
#include <layers/Resource/ResourceFileSystem.hpp>
#include <layers/Logic/VirtualFileSystem.hpp>

#if DEBUG

static arx_debug_view_t archiveViews[2];
static arx_debug_view_t vfsViews[3];

void InitRamFileSystemManager::PublishDebug(DebugManager& manager)
{
    const char* names[] = {"contents", "entry"};

    for (size_t index = 0; index < 2; index++)
    {
        archiveViews[index] = {names[index], "InitRamDebugStats", "arx.filesystem.archive", 1,
                               &DebugStats, sizeof(DebugStats), 0, MAX_ARCHIVES};
    }

    manager.Register("initramfs", "InitRamDebugStats", &DebugStats, sizeof(DebugStats),
                     1, &DebugStats.group, archiveViews, 2);
}

bool ResourceFileSystem::DebugNativeBackend(const ResourceLayerFileSystemCaps* caps)
{
    return caps != nullptr
        && caps->MountFilesystem == MountFilesystemThunk
        && caps->Lookup == LookupThunk
        && caps->GetNodeInfo == GetNodeInfoThunk
        && caps->ReadDirectory == ReadDirectoryThunk;
}

void ResourceFileSystem::PublishDebug(DebugManager& manager)
{
    manager.Register("resource-fs", "ResourceFileSystemDebugStats", &DebugStats,
                     sizeof(DebugStats), 1, &DebugStats.group);
}

void VirtualFileSystem::PublishDebug(DebugManager& manager)
{
    const char* names[] = {"mounts", "cache", "contents"};

    for (size_t index = 0; index < 3; index++)
    {
        vfsViews[index] = {names[index], "VfsDebugStats", "arx.filesystem.namespace", 1,
                           &DebugStats, sizeof(DebugStats), 0, 0};
    }

    manager.Register("vfs", "VfsDebugStats", &DebugStats, sizeof(DebugStats),
                     1, &DebugStats.group, vfsViews, 3);
}

#endif
