#pragma once

#include "TestCase.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

// Descriptor entries may only be borrowed under FileDescriptorGuard. Retain
// the file before leaving that transaction so a test can safely use its APIs.
class TestFile
{
public:
    TestFile(TestCase& test, ResourceLayerCaps* resources, VirtualFileSystem* vfs, process_t* process, int64_t descriptor)
        : File(nullptr), Vfs(vfs)
    {
        FileDescriptorGuard guard(process);
        file_descriptor_t*  entry = resources->processManager->GetFileDescriptor(process, descriptor);

        if (test.Check(entry && entry->file, "descriptor resolves to live file"))
        {
            auto* file = static_cast<file_t*>(entry->file);

            if (test.Check(Vfs->Retain(file), "retain descriptor file for test"))
            {
                File = file;
            }
        }
    }

    ~TestFile()
    {
        Reset();
    }

    void Reset()
    {
        if (File)
        {
            if (Vfs->Close(File) != 0)
            {
                SelfTestManager::Get().AbortUnsafe("cannot release retained test file");
            }

            File = nullptr;
        }
    }

    file_t* Get() const
    {
        return File;
    }

    TestFile(const TestFile&) = delete;
    TestFile& operator=(const TestFile&) = delete;

private:
    file_t*            File;
    VirtualFileSystem* Vfs;
};
