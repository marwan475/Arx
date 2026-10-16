#pragma once

#include "layers/Logic/VirtualFileSystem.hpp"

#include <klib/klib.h>

// A borrowed in-memory backend for public VFS/ELF contracts. The exported
// manager remains under test; only its file input is synthetic. Never release
// the final reference through Close: these objects have automatic storage.
struct MemoryFileFixture
{
    explicit MemoryFileFixture(VirtualFileSystem* vfs, size_t size = 0)
    {
        memset(this, 0, sizeof(*this));
        vfs->InitializeAnonymousInode(&inode, INODE_REGULAR, 0600);
        inode.size       = size;
        operations.Read  = Read;
        operations.Write = Write;
        operations.Seek  = Seek;
        file.owner       = vfs;
        file.inode       = &inode;
        file.operations  = &operations;
        file.privateData = this;
        file.refCount    = 1;
        file.statusFlags = 2;
    }

    static int64_t Read(file_t* file, void* output, uint64_t count)
    {
        auto* fixture = static_cast<MemoryFileFixture*>(file->privateData);

        if (file->offset >= fixture->inode.size)
        {
            return 0;
        }

        uint64_t available = fixture->inode.size - file->offset;
        count              = count < available ? count : available;
        memcpy(output, fixture->bytes + file->offset, count);
        file->offset += count;
        return count;
    }

    static int64_t Write(file_t* file, const void* input, uint64_t count)
    {
        auto* fixture = static_cast<MemoryFileFixture*>(file->privateData);

        if (file->offset > sizeof(fixture->bytes) || count > sizeof(fixture->bytes) - file->offset)
        {
            return -28;
        }

        memcpy(fixture->bytes + file->offset, input, count);
        file->offset += count;

        if (file->offset > fixture->inode.size)
        {
            fixture->inode.size = file->offset;
        }

        return count;
    }

    static int64_t Seek(file_t* file, int64_t offset, int whence)
    {
        if (whence != 0 || offset < 0 || static_cast<uint64_t>(offset) > 256)
        {
            return -22;
        }

        file->offset = offset;
        return offset;
    }

    uint8_t           bytes[256];
    inode_t           inode;
    file_operations_t operations;
    file_t            file;
};
