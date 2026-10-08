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
    if (path == nullptr)
    {
        return LINUX_EFAULT;
    }

    vfs_path_t start            = {};
    bool       nonDirectoryBase = false;
    if (!resolve_open_start_path(LogicCaps->virtualFileSystem, currentProcess, frame, path, &start, &nonDirectoryBase))
    {
        return LINUX_EBADF;
    }

    if (nonDirectoryBase)
    {
        return LINUX_ENOTDIR;
    }

    file_t*    file  = LogicCaps->virtualFileSystem->Open(start, path, frame->arg2);
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
    (void) frame;
    return (uint64_t) -38;
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
    if (buffer == nullptr)
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
    if (buffer == nullptr)
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
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleChdirRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleFchdirRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
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
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleStatRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
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
    if (statBuffer == nullptr)
    {
        return LINUX_EFAULT;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    if (file->inode == nullptr)
    {
        return LINUX_EIO;
    }

    linux_stat_t statData = {};
    statData.st_ino       = file->inode->inodeNumber;
    statData.st_dev       = 1;
    statData.st_nlink     = 1;
    statData.st_mode      = linux_mode_for_inode(file->inode);
    statData.st_uid       = 0;
    statData.st_gid       = 0;
    statData.st_rdev      = 0;
    statData.st_size      = (int64_t) file->inode->size;
    statData.st_blksize   = 4096;
    statData.st_blocks    = (int64_t) ((file->inode->size + 511ULL) / 512ULL);

    *statBuffer = statData;
    return 0;
}

uint64_t VfsRequestManager::HandleLstatRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
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
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleReadlinkRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
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
    (void) frame;
    return (uint64_t) -38;
}

uint64_t VfsRequestManager::HandleSymlinkRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
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
