#include "layers/Request/VfsRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

struct linux_timespec_t
{
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_stat_t
{
    uint64_t         st_dev;
    uint64_t         st_ino;
    uint64_t         st_nlink;
    uint32_t         st_mode;
    uint32_t         st_uid;
    uint32_t         st_gid;
    int32_t          pad0;
    uint64_t         st_rdev;
    int64_t          st_size;
    int64_t          st_blksize;
    int64_t          st_blocks;
    linux_timespec_t st_atim;
    linux_timespec_t st_mtim;
    linux_timespec_t st_ctim;
    int64_t          reserved[3];
};

constexpr uint32_t LINUX_S_IFREG = 0100000U;
constexpr uint32_t LINUX_S_IFDIR = 0040000U;
constexpr uint32_t LINUX_S_IFLNK = 0120000U;

constexpr uint64_t LINUX_O_WRONLY = 01ULL;
constexpr uint64_t LINUX_O_CREAT  = 0100ULL;
constexpr uint64_t LINUX_O_TRUNC  = 01000ULL;

constexpr int64_t  LINUX_AT_SYMLINK_NOFOLLOW = 0x100;
constexpr int64_t  LINUX_AT_EMPTY_PATH       = 0x1000;

static uint32_t linux_mode_for_inode(const inode_t* inode)
{
    if (inode == nullptr)
    {
        return 0;
    }

    switch (inode->type)
    {
        case INODE_DIRECTORY:
            return LINUX_S_IFDIR | 0755U;
        case INODE_SYMLINK:
            return LINUX_S_IFLNK | 0777U;
        default:
            return LINUX_S_IFREG | 0644U;
    }
}

static void fill_linux_stat_from_inode(const inode_t* inode, linux_stat_t* statBuffer)
{
    if (inode == nullptr || statBuffer == nullptr)
    {
        return;
    }

    linux_stat_t statData = {};
    statData.st_ino       = inode->inodeNumber;
    statData.st_dev       = 1;
    statData.st_nlink     = 1;
    statData.st_mode      = linux_mode_for_inode(inode);
    statData.st_uid       = 0;
    statData.st_gid       = 0;
    statData.st_rdev      = 0;
    statData.st_size      = (int64_t) inode->size;
    statData.st_blksize   = 4096;
    statData.st_blocks    = (int64_t) ((inode->size + 511ULL) / 512ULL);

    *statBuffer = statData;
}

static bool build_path_from_cwd(VirtualFileSystem* virtualFileSystem, process_t* process, const char* path, char* outPath, size_t outPathSize)
{
    if (virtualFileSystem == nullptr || process == nullptr || path == nullptr || outPath == nullptr || outPathSize == 0)
    {
        return false;
    }

    return virtualFileSystem->JoinPath(process->cwdPath, path, outPath, outPathSize);
}

static bool is_obviously_invalid_user_pointer(const void* pointer)
{
    const uintptr_t address = (uintptr_t) pointer;
    return address == 0 || address < 0x1000ULL;
}

static bool resolve_open_start_path(VirtualFileSystem* virtualFileSystem, process_t* currentProcess, const arch_syscall_frame_t* frame, const char* path,
                                    vfs_path_t* outStart, bool* outNonDirectoryDirfd)
{
    if (virtualFileSystem == nullptr || currentProcess == nullptr || frame == nullptr || path == nullptr || outStart == nullptr ||
        outNonDirectoryDirfd == nullptr)
    {
        return false;
    }

    *outNonDirectoryDirfd = false;

    vfs_path_t start = {};

    if (path[0] == '/')
    {
        *outStart = start;
        return true;
    }

    const int64_t dirfd = (int64_t) frame->arg0;
    if (dirfd == LINUX_AT_FDCWD)
    {
        // CWD tracking is not implemented yet; treat AT_FDCWD as VFS root.
        if (!virtualFileSystem->ResolvePath(start, "/", &start))
        {
            return false;
        }

        *outStart = start;
        return true;
    }

    if (dirfd < 0)
    {
        return false;
    }

    const uint64_t fd = (uint64_t) dirfd;
    if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
    {
        return false;
    }

    file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
    if (descriptor->file == nullptr)
    {
        return false;
    }

    file_t* baseFile = static_cast<file_t*>(descriptor->file);
    if (baseFile->inode == nullptr || baseFile->inode->type != INODE_DIRECTORY)
    {
        *outNonDirectoryDirfd = true;
        return true;
    }

    *outStart = baseFile->path;
    return true;
}

static bool resolve_relative_base_for_dirfd(process_t* currentProcess, int64_t dirfd, vfs_path_t* outStart, bool* outNonDirectoryDirfd)
{
    if (currentProcess == nullptr || outStart == nullptr || outNonDirectoryDirfd == nullptr)
    {
        return false;
    }

    *outNonDirectoryDirfd = false;

    if (dirfd == LINUX_AT_FDCWD)
    {
        *outStart = {};
        return true;
    }

    if (dirfd < 0)
    {
        return false;
    }

    const uint64_t fd = (uint64_t) dirfd;
    if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
    {
        return false;
    }

    file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
    if (descriptor->file == nullptr)
    {
        return false;
    }

    file_t* baseFile = static_cast<file_t*>(descriptor->file);
    if (baseFile->inode == nullptr || baseFile->inode->type != INODE_DIRECTORY)
    {
        *outNonDirectoryDirfd = true;
        return true;
    }

    *outStart = baseFile->path;
    return true;
}


static uint64_t normalize_vfs_result(int64_t result)
{
    if (result >= 0)
    {
        return (uint64_t) result;
    }

    if (result == -1)
    {
        return LINUX_EIO;
    }

    if (result >= -4095)
    {
        return (uint64_t) result;
    }

    return LINUX_EIO;
}

VfsRequestManager::VfsRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps)
    : ResourceCaps(resourceLayerCaps), LogicCaps(logicLayerCaps)
{
}

uint64_t VfsRequestManager::HandleOpenatRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const char* path = (const char*) (uintptr_t) frame->arg1;
    if (is_obviously_invalid_user_pointer(path))
    {
        return LINUX_EFAULT;
    }

    const int64_t dirfd = (int64_t) frame->arg0;
    char          cwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char*   effectivePath = path;
    if (path[0] != '/' && dirfd == LINUX_AT_FDCWD)
    {
        if (!build_path_from_cwd(LogicCaps->virtualFileSystem, currentProcess, path, cwdResolvedPath, sizeof(cwdResolvedPath)))
        {
            return LINUX_ENAMETOOLONG;
        }

        effectivePath = cwdResolvedPath;
    }

    vfs_path_t start            = {};
    bool       nonDirectoryBase = false;
    if (!resolve_open_start_path(LogicCaps->virtualFileSystem, currentProcess, frame, effectivePath, &start, &nonDirectoryBase))
    {
        return LINUX_EBADF;
    }

    if (nonDirectoryBase)
    {
        return LINUX_ENOTDIR;
    }

    file_t*    file  = LogicCaps->virtualFileSystem->Open(start, effectivePath, frame->arg2);
    if (file == nullptr)
    {
        return LINUX_ENOENT;
    }

    int64_t fd = ResourceCaps->processManager->AddFileDescriptor(currentProcess, static_cast<file_handle_t>(file), FD_FLAG_NONE);
    if (fd < 0)
    {
        (void) LogicCaps->virtualFileSystem->Close(file);
        return LINUX_EIO;
    }

    return (uint64_t) fd;
}

uint64_t VfsRequestManager::HandleOpenRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t openatFrame = *frame;
    openatFrame.arg1                 = frame->arg0;
    openatFrame.arg2                 = frame->arg1;
    openatFrame.arg3                 = frame->arg2;
    openatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;

    return HandleOpenatRequest(&openatFrame);
}

uint64_t VfsRequestManager::HandleCreatRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t openatFrame = {};
    openatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    openatFrame.arg1                 = frame->arg0;
    openatFrame.arg2                 = LINUX_O_WRONLY | LINUX_O_CREAT | LINUX_O_TRUNC;
    openatFrame.arg3                 = frame->arg1;

    return HandleOpenatRequest(&openatFrame);
}

uint64_t VfsRequestManager::HandleCloseRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t fd = frame->arg0;
    if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
    {
        return LINUX_EBADF;
    }

    file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
    if (descriptor->file == nullptr)
    {
        return LINUX_EBADF;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    descriptor->file  = nullptr;
    descriptor->flags = FD_FLAG_NONE;

    int64_t closeResult = LogicCaps->virtualFileSystem->Close(file);
    return normalize_vfs_result(closeResult);
}

uint64_t VfsRequestManager::HandleClose_rangeRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleMkdiratRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleMkdirRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleReadRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t fd = frame->arg0;
    if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
    {
        return LINUX_EBADF;
    }

    file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
    if (descriptor->file == nullptr)
    {
        return LINUX_EBADF;
    }

    const uint64_t count = frame->arg2;
    if (count == 0)
    {
        return 0;
    }

    void* buffer = (void*) (uintptr_t) frame->arg1;
    if (is_obviously_invalid_user_pointer(buffer))
    {
        return LINUX_EFAULT;
    }

    file_t* file    = static_cast<file_t*>(descriptor->file);
    int64_t result  = LogicCaps->virtualFileSystem->Read(file, buffer, count);
    return normalize_vfs_result(result);
}

uint64_t VfsRequestManager::HandleWriteRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t fd = frame->arg0;
    if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
    {
        return LINUX_EBADF;
    }

    file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
    if (descriptor->file == nullptr)
    {
        return LINUX_EBADF;
    }

    const uint64_t count = frame->arg2;
    if (count == 0)
    {
        return 0;
    }

    const void* buffer = (const void*) (uintptr_t) frame->arg1;
    if (is_obviously_invalid_user_pointer(buffer))
    {
        return LINUX_EFAULT;
    }

    file_t* file    = static_cast<file_t*>(descriptor->file);
    int64_t result  = LogicCaps->virtualFileSystem->Write(file, buffer, count);
    return normalize_vfs_result(result);
}

uint64_t VfsRequestManager::HandlePread64Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandlePwrite64Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleReadvRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleWritevRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandlePreadvRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandlePwritevRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleLseekRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t fd = frame->arg0;
    if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
    {
        return LINUX_EBADF;
    }

    file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
    if (descriptor->file == nullptr)
    {
        return LINUX_EBADF;
    }

    const int whence = (int) frame->arg2;
    if (whence < 0 || whence > 2)
    {
        return LINUX_EINVAL;
    }

    file_t* file   = static_cast<file_t*>(descriptor->file);
    int64_t offset = (int64_t) frame->arg1;
    int64_t result = LogicCaps->virtualFileSystem->Seek(file, offset, whence);
    return normalize_vfs_result(result);
}

uint64_t VfsRequestManager::HandleGetcwdRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    char*          buffer     = (char*) (uintptr_t) frame->arg0;
    const uint64_t bufferSize = frame->arg1;
    if (is_obviously_invalid_user_pointer(buffer))
    {
        return LINUX_EFAULT;
    }

    if (bufferSize == 0)
    {
        return LINUX_EINVAL;
    }

    const size_t cwdLength = strlen(currentProcess->cwdPath) + 1;
    if (cwdLength > bufferSize)
    {
        return LINUX_ERANGE;
    }

    memcpy(buffer, currentProcess->cwdPath, cwdLength);
    return (uint64_t) (uintptr_t) buffer;
}

uint64_t VfsRequestManager::HandleChdirRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const char* path = (const char*) (uintptr_t) frame->arg0;
    if (is_obviously_invalid_user_pointer(path))
    {
        return LINUX_EFAULT;
    }

    char resolvedPathBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    if (!build_path_from_cwd(LogicCaps->virtualFileSystem, currentProcess, path, resolvedPathBuffer, sizeof(resolvedPathBuffer)))
    {
        return LINUX_ENAMETOOLONG;
    }

    vfs_path_t start    = {};
    vfs_path_t resolved = {};
    if (!LogicCaps->virtualFileSystem->ResolvePath(start, resolvedPathBuffer, &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
    {
        return LINUX_ENOENT;
    }

    if (resolved.dentry->inode->type != INODE_DIRECTORY)
    {
        return LINUX_ENOTDIR;
    }

    if (!LogicCaps->virtualFileSystem->BuildAbsolutePathFromDentry(resolved.dentry, currentProcess->cwdPath, sizeof(currentProcess->cwdPath)))
    {
        return LINUX_ENAMETOOLONG;
    }

    return 0;
}

uint64_t VfsRequestManager::HandleFchdirRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t fd = frame->arg0;
    if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
    {
        return LINUX_EBADF;
    }

    file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
    if (descriptor->file == nullptr)
    {
        return LINUX_EBADF;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    if (file->inode == nullptr || file->inode->type != INODE_DIRECTORY)
    {
        return LINUX_ENOTDIR;
    }

    if (!LogicCaps->virtualFileSystem->BuildAbsolutePathFromDentry(file->path.dentry, currentProcess->cwdPath, sizeof(currentProcess->cwdPath)))
    {
        return LINUX_ENAMETOOLONG;
    }

    return 0;
}

uint64_t VfsRequestManager::HandleGetdents64Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleUnlinkatRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleUnlinkRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleRmdirRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFcntlRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleDupRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleDup2Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleDup3Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleNewfstatatRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    linux_stat_t* statBuffer = (linux_stat_t*) (uintptr_t) frame->arg2;
    if (is_obviously_invalid_user_pointer(statBuffer))
    {
        return LINUX_EFAULT;
    }

    const int64_t flags = (int64_t) frame->arg3;
    if ((flags & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0)
    {
        return LINUX_EINVAL;
    }

    const int64_t dirfd = (int64_t) frame->arg0;
    const char*   path  = (const char*) (uintptr_t) frame->arg1;
    if (is_obviously_invalid_user_pointer(path))
    {
        return LINUX_EFAULT;
    }

    if (path[0] == '\0')
    {
        if ((flags & LINUX_AT_EMPTY_PATH) == 0)
        {
            return LINUX_ENOENT;
        }

        if (dirfd < 0)
        {
            return LINUX_EBADF;
        }

        const uint64_t fd = (uint64_t) dirfd;
        if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
        {
            return LINUX_EBADF;
        }

        file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
        if (descriptor->file == nullptr)
        {
            return LINUX_EBADF;
        }

        file_t* file = static_cast<file_t*>(descriptor->file);
        if (file->inode == nullptr)
        {
            return LINUX_EIO;
        }

        fill_linux_stat_from_inode(file->inode, statBuffer);
        return 0;
    }

    vfs_path_t start            = {};
    bool       nonDirectoryBase = false;
    char       cwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectivePath = path;

    if (path[0] != '/' && dirfd == LINUX_AT_FDCWD)
    {
        if (!build_path_from_cwd(LogicCaps->virtualFileSystem, currentProcess, path, cwdResolvedPath, sizeof(cwdResolvedPath)))
        {
            return LINUX_ENAMETOOLONG;
        }

        effectivePath = cwdResolvedPath;
    }

    if ((flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0)
    {
        inode_t* inode = nullptr;
        const int64_t statResult = LogicCaps->virtualFileSystem->StatNoFollow(start, effectivePath, &inode);
        if (statResult < 0 || inode == nullptr)
        {
            return normalize_vfs_result(statResult);
        }

        fill_linux_stat_from_inode(inode, statBuffer);
        return 0;
    }

    if (effectivePath[0] == '/')
    {
        start = {};
    }
    else
    {
        if (!resolve_relative_base_for_dirfd(currentProcess, dirfd, &start, &nonDirectoryBase))
        {
            return LINUX_EBADF;
        }

        if (nonDirectoryBase)
        {
            return LINUX_ENOTDIR;
        }
    }

    vfs_path_t resolved = {};
    if (!LogicCaps->virtualFileSystem->ResolvePath(start, effectivePath, &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
    {
        return LINUX_ENOENT;
    }

    fill_linux_stat_from_inode(resolved.dentry->inode, statBuffer);
    return 0;
}

uint64_t VfsRequestManager::HandleStatRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t statatFrame = {};
    statatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    statatFrame.arg1                 = frame->arg0;
    statatFrame.arg2                 = frame->arg1;
    statatFrame.arg3                 = 0;
    return HandleNewfstatatRequest(&statatFrame);
}

uint64_t VfsRequestManager::HandleFstatRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t fd = frame->arg0;
    if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
    {
        return LINUX_EBADF;
    }

    file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
    if (descriptor->file == nullptr)
    {
        return LINUX_EBADF;
    }

    linux_stat_t* statBuffer = (linux_stat_t*) (uintptr_t) frame->arg1;
    if (is_obviously_invalid_user_pointer(statBuffer))
    {
        return LINUX_EFAULT;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    if (file->inode == nullptr)
    {
        return LINUX_EIO;
    }

    fill_linux_stat_from_inode(file->inode, statBuffer);
    return 0;
}

uint64_t VfsRequestManager::HandleLstatRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t statatFrame = {};
    statatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    statatFrame.arg1                 = frame->arg0;
    statatFrame.arg2                 = frame->arg1;
    statatFrame.arg3                 = (uint64_t) LINUX_AT_SYMLINK_NOFOLLOW;
    return HandleNewfstatatRequest(&statatFrame);
}

uint64_t VfsRequestManager::HandleRenameatRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleRenameRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleReadlinkatRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int64_t dirfd = (int64_t) frame->arg0;
    const char*   path  = (const char*) (uintptr_t) frame->arg1;
    char*         buffer = (char*) (uintptr_t) frame->arg2;
    const uint64_t bufferSize = frame->arg3;

    if (is_obviously_invalid_user_pointer(path) || is_obviously_invalid_user_pointer(buffer))
    {
        return LINUX_EFAULT;
    }

    if (bufferSize == 0)
    {
        return LINUX_EINVAL;
    }

    char        effectivePathBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectivePath = path;
    if (path[0] != '/')
    {
        if (dirfd == LINUX_AT_FDCWD)
        {
            if (!build_path_from_cwd(LogicCaps->virtualFileSystem, currentProcess, path, effectivePathBuffer, sizeof(effectivePathBuffer)))
            {
                return LINUX_ENAMETOOLONG;
            }

            effectivePath = effectivePathBuffer;
        }
        else
        {
            if (dirfd < 0)
            {
                return LINUX_EBADF;
            }

            const uint64_t fd = (uint64_t) dirfd;
            if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
            {
                return LINUX_EBADF;
            }

            file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
            if (descriptor->file == nullptr)
            {
                return LINUX_EBADF;
            }

            file_t* baseFile = static_cast<file_t*>(descriptor->file);
            if (baseFile->inode == nullptr || baseFile->inode->type != INODE_DIRECTORY)
            {
                return LINUX_ENOTDIR;
            }

            char basePath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
            if (!LogicCaps->virtualFileSystem->BuildAbsolutePathFromDentry(baseFile->path.dentry, basePath, sizeof(basePath)))
            {
                return LINUX_ENAMETOOLONG;
            }

            if (!LogicCaps->virtualFileSystem->JoinPath(basePath, path, effectivePathBuffer, sizeof(effectivePathBuffer)))
            {
                return LINUX_ENAMETOOLONG;
            }

            effectivePath = effectivePathBuffer;
        }
    }

    return normalize_vfs_result(LogicCaps->virtualFileSystem->Readlink({}, effectivePath, buffer, bufferSize));
}

uint64_t VfsRequestManager::HandleReadlinkRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t readlinkatFrame = {};
    readlinkatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    readlinkatFrame.arg1                 = frame->arg0;
    readlinkatFrame.arg2                 = frame->arg1;
    readlinkatFrame.arg3                 = frame->arg2;
    return HandleReadlinkatRequest(&readlinkatFrame);
}

uint64_t VfsRequestManager::HandleIoctlRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleLinkatRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleLinkRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleSymlinkatRequest(const arch_syscall_frame_t* frame)
{
    kprintf("Arx kernel: rq symlinkat enter\n");

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const char* target   = (const char*) (uintptr_t) frame->arg0;
    const int64_t dirfd  = (int64_t) frame->arg1;
    const char* linkPath = (const char*) (uintptr_t) frame->arg2;

    if (is_obviously_invalid_user_pointer(target) || is_obviously_invalid_user_pointer(linkPath))
    {
        return LINUX_EFAULT;
    }

    kprintf("Arx kernel: rq symlinkat args target=%s link=%s dirfd=%lld\n", target, linkPath, (long long) dirfd);

    char        effectiveLinkPathBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectiveLinkPath = linkPath;
    if (linkPath[0] != '/')
    {
        if (dirfd == LINUX_AT_FDCWD)
        {
            if (!build_path_from_cwd(LogicCaps->virtualFileSystem, currentProcess, linkPath, effectiveLinkPathBuffer, sizeof(effectiveLinkPathBuffer)))
            {
                return LINUX_ENAMETOOLONG;
            }

            effectiveLinkPath = effectiveLinkPathBuffer;
        }
        else
        {
            if (dirfd < 0)
            {
                return LINUX_EBADF;
            }

            const uint64_t fd = (uint64_t) dirfd;
            if (fd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors == nullptr)
            {
                return LINUX_EBADF;
            }

            file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];
            if (descriptor->file == nullptr)
            {
                return LINUX_EBADF;
            }

            file_t* baseFile = static_cast<file_t*>(descriptor->file);
            if (baseFile->inode == nullptr || baseFile->inode->type != INODE_DIRECTORY)
            {
                return LINUX_ENOTDIR;
            }

            char basePath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
            if (!LogicCaps->virtualFileSystem->BuildAbsolutePathFromDentry(baseFile->path.dentry, basePath, sizeof(basePath)))
            {
                return LINUX_ENAMETOOLONG;
            }

            if (!LogicCaps->virtualFileSystem->JoinPath(basePath, linkPath, effectiveLinkPathBuffer, sizeof(effectiveLinkPathBuffer)))
            {
                return LINUX_ENAMETOOLONG;
            }

            effectiveLinkPath = effectiveLinkPathBuffer;
        }
    }

    kprintf("Arx kernel: rq symlinkat call vfs path=%s\n", effectiveLinkPath);
    const int64_t result = LogicCaps->virtualFileSystem->Symlink({}, target, effectiveLinkPath);
    kprintf("Arx kernel: rq symlinkat return vfs=%lld\n", (long long) result);
    return normalize_vfs_result(result);
}

uint64_t VfsRequestManager::HandleSymlinkRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t symlinkatFrame = {};
    symlinkatFrame.arg0                 = frame->arg0;
    symlinkatFrame.arg1                 = (uint64_t) LINUX_AT_FDCWD;
    symlinkatFrame.arg2                 = frame->arg1;
    return HandleSymlinkatRequest(&symlinkatFrame);
}

uint64_t VfsRequestManager::HandleFaccessatRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFaccessat2Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleAccessRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFchownatRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleChownRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFchownRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleLchownRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFchmodat2Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFchmodatRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleChmodRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFchmodRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleMountRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleStatfsRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFstatfsRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleMknodatRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleMknodRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleTruncateRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFtruncateRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandlePipeRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandlePipe2Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleMemfd_createRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}
