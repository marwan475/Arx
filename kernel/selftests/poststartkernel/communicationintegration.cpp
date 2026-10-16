#include <selftests/TestFile.hpp>
#include "integrationtests.hpp"

#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SocketManager.hpp"
#include "layers/Logic/InterProcessCommunicationManager.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"

void test_communication_integration(ResourceLayerCaps* r, LogicLayerCaps* l)
{
    {
        TestCase t("poststartkernel/ipc/cross_process_pipe");

        if (!l || !l->interProcessCommunicationManager || !l->virtualFileSystem)
        {
            t.Block("IPC or VFS unavailable");
            return;
        }

        TestFixture writer(r);

        if (!writer.Valid(t) || !t.Check(writer.Map(), "map writer buffer"))
        {
            return;
        }

        if (!t.Equal(l->interProcessCommunicationManager->CreatePipe(TestFixture::Buffer, 04000), 0, "create pipe"))
        {
            return;
        }

        int32_t pair[2] = {-1, -1};

        if (!t.Check(safe_read_user(TestFixture::Buffer, pair, sizeof(pair)), "read pipe handles"))
        {
            return;
        }

        TestFile writeHandle(t, r, l->virtualFileSystem, writer.Process, pair[1]);
        auto* output = writeHandle.Get();
        TestFile readHandle(t, r, l->virtualFileSystem, writer.Process, pair[0]);
        auto* input = readHandle.Get();

        if (!output || !input)
        {
            return;
        }
        uint64_t message = 0x31415926;
        uint64_t actual = 0;
        t.Check(safe_write_user(TestFixture::Buffer + 64, &message, 8), "seed writer payload");
        t.Equal(output->operations->WriteUser(output, TestFixture::Buffer + 64, 8), 8, "write from first process");
        TestFixture reader(r);

        if (!reader.Valid(t) || !t.Check(reader.Map(), "map reader buffer"))
        {
            return;
        }

        if (!t.Check(l->virtualFileSystem->Retain(input), "retain transferred read handle"))
        {
            return;
        }

        int64_t fd = r->processManager->AddFileDescriptor(reader.Process, input, 0, VirtualFileSystem::ReleaseFileHandle);

        if (!t.Check(fd >= 0, "install read handle into second process"))
        {
            l->virtualFileSystem->Close(input);
            return;
        }

        t.Check(reader.Process->id != writer.Process->id, "distinct process identities");
        t.Equal(input->operations->ReadUser(input, TestFixture::Buffer, 8), 8, "read into second process address space");
        t.Check(safe_read_user(TestFixture::Buffer, &actual, 8), "copy recipient payload");
        t.Equal(actual, message, "cross-process pipe preserves data");
    }

    {
        TestCase t("poststartkernel/sockets/cross_process_local_pair");

        if (!l || !l->socketManager || !l->virtualFileSystem)
        {
            t.Block("socket or VFS unavailable");
            return;
        }

        TestFixture sender(r);

        if (!sender.Valid(t) || !t.Check(sender.Map(), "map sender buffer"))
        {
            return;
        }

        if (!t.Equal(l->socketManager->Socketpair(1, 1 | 04000, 0, TestFixture::Buffer), 0, "create local stream pair"))
        {
            return;
        }

        int32_t pair[2];

        if (!t.Check(safe_read_user(TestFixture::Buffer, pair, sizeof(pair)), "read endpoint handles"))
        {
            return;
        }

        TestFile peerHandle(t, r, l->virtualFileSystem, sender.Process, pair[1]);
        auto* endpoint = peerHandle.Get();

        if (!endpoint)
        {
            return;
        }
        uint64_t message  = 0x27182818;
        uint64_t actual = 0;
        t.Check(safe_write_user(TestFixture::Buffer + 64, &message, 8), "seed socket payload");
        t.Equal(l->socketManager->Sendto(pair[0], TestFixture::Buffer + 64, 8, 0x40, 0, 0), 8, "send from first identity");
        TestFixture receiver(r);

        if (!receiver.Valid(t) || !t.Check(receiver.Map(), "map receiver buffer"))
        {
            return;
        }

        if (!t.Check(l->virtualFileSystem->Retain(endpoint), "retain transferred socket"))
        {
            return;
        }

        int64_t fd = r->processManager->AddFileDescriptor(receiver.Process, endpoint, 0, VirtualFileSystem::ReleaseFileHandle);

        if (!t.Check(fd >= 0, "install endpoint into second process"))
        {
            l->virtualFileSystem->Close(endpoint);
            return;
        }

        t.Equal(l->socketManager->Recvfrom(fd, TestFixture::Buffer, 8, 0x40, 0, 0), 8, "receive under second identity");
        t.Check(safe_read_user(TestFixture::Buffer, &actual, 8), "copy receiver payload");
        t.Equal(actual, message, "cross-process socket preserves data");
    }
}
