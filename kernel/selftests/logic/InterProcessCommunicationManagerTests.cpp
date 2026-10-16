#include <selftests/TestFile.hpp>
#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/InterProcessCommunicationManager.hpp"

void test_InterProcessCommunicationManager(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->interProcessCommunicationManager : nullptr;
    ManagerSuite suite("logic/InterProcessCommunicationManager", m != nullptr);
    suite.Run("logic/InterProcessCommunicationManager/pipe_nonblocking", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t) || !t.Check(f.Map(), "map pipe buffers"))
        {
            return;
        }

        if (!t.Equal(m->CreatePipe(TestFixture::Buffer, 04000), 0, "create nonblocking pipe"))
        {
            return;
        }

        int32_t pair[2] = {-1, -1};

        if (!t.Check(safe_read_user(TestFixture::Buffer, pair, sizeof(pair)), "copy pipe descriptors"))
        {
            return;
        }

        TestFile readHandle(t, resources, logic->virtualFileSystem, f.Process, pair[0]);
        auto* reader = readHandle.Get();
        TestFile writeHandle(t, resources, logic->virtualFileSystem, f.Process, pair[1]);
        auto* writer = writeHandle.Get();

        if (!reader || !writer)
        {
            return;
        }
        uint64_t value  = 0x1234;
        uint64_t actual = 0;
        t.Check(safe_write_user(TestFixture::Buffer + 64, &value, 8), "initialize pipe payload");
        t.Equal(reader->operations->ReadUser(reader, TestFixture::Buffer + 128, 8), LINUX_EAGAIN, "empty read does not block");
        t.Equal(writer->operations->WriteUser(writer, TestFixture::Buffer + 64, 8), 8, "pipe write");
        t.Equal(reader->operations->ReadUser(reader, TestFixture::Buffer + 128, 8), 8, "pipe read");
        t.Check(safe_read_user(TestFixture::Buffer + 128, &actual, 8), "copy pipe result");
        t.Equal(actual, value, "pipe preserves payload");
        writeHandle.Reset();
        t.Check(f.Close(pair[1]), "close writer");
        t.Equal(reader->operations->ReadUser(reader, TestFixture::Buffer + 128, 8), 0, "last writer closure produces EOF");
    });

    suite.Run("logic/InterProcessCommunicationManager/memory_file", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t) || !t.Check(f.Map(), "map memory-file name"))
        {
            return;
        }

        const char name[] = "manager-unit";
        t.Check(safe_write_user(TestFixture::Buffer, name, sizeof(name)), "copy file name");
        int64_t fd = m->CreateMemoryFile(TestFixture::Buffer, 0);

        if (!t.Check(fd >= 0, "create anonymous memory file"))
        {
            return;
        }

        TestFile retained(t, resources, logic->virtualFileSystem, f.Process, fd);
        auto* file = retained.Get();

        if (!file)
        {
            return;
        }
        const char text[] = "round-trip";
        char       actual[sizeof(text)] = {};
        t.Equal(logic->virtualFileSystem->Write(file, text, sizeof(text)), sizeof(text), "memory file write");
        t.Equal(logic->virtualFileSystem->Seek(file, 0, 0), 0, "rewind memory file");
        t.Equal(logic->virtualFileSystem->Read(file, actual, sizeof(actual)), sizeof(actual), "memory file read");
        t.Check(memcmp(text, actual, sizeof(text)) == 0, "memory file content");
    });

    suite.Run("logic/InterProcessCommunicationManager/flags_validation", [&](TestCase& t)
    {
        t.Equal(m->CreatePipe(0, 0x80000000), LINUX_EINVAL, "unknown pipe flags");
        t.Equal(m->CreateMemoryFile(0, 0x80000000), LINUX_EINVAL, "unknown memory-file flags");
    });
}
