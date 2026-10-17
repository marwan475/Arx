#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SocketManager.hpp"

void test_SocketManager(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->socketManager : nullptr;
    ManagerSuite suite("logic/SocketManager", m != nullptr);
    suite.Run("logic/SocketManager/protocol_validation", [&](TestCase& t)
    {
        t.Equal(m->Socket(2, 1, 0), (uint64_t) -97, "unsupported address family");
        t.Equal(m->Socket(1, 0, 0), (uint64_t) -94, "unsupported socket type");
        t.Equal(m->Socket(1, 1, 1), (uint64_t) -93, "unsupported local protocol");
    });

    suite.Run("logic/SocketManager/local_pair_transfer", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t) || !t.Check(f.Map(), "map socket buffers"))
        {
            return;
        }

        t.Equal(m->Socketpair(1, 1 | 04000, 0, TestFixture::Buffer), 0, "create nonblocking local pair");
        int32_t pair[2] = {-1, -1};

        if (!t.Check(safe_read_user(TestFixture::Buffer, pair, sizeof(pair)) && pair[0] >= 0 && pair[1] >= 0, "read pair descriptors"))
        {
            return;
        }

        uint64_t payload = 0x123456789abcdef0;

        uint64_t actual = 0;
        t.Check(safe_write_user(TestFixture::Buffer + 64, &payload, 8), "initialize send buffer");
        t.Equal(m->Sendto(pair[0], TestFixture::Buffer + 64, 8, 0x40, 0, 0), 8, "send through local pair");
        t.Equal(m->Recvfrom(pair[1], TestFixture::Buffer + 128, 8, 0x40, 0, 0), 8, "receive through pair");
        t.Check(safe_read_user(TestFixture::Buffer + 128, &actual, 8), "copy received bytes");
        t.Equal(actual, payload, "socket payload preserved");
        t.Equal(m->Recvfrom(pair[1], TestFixture::Buffer + 128, 8, 0x40, 0, 0), LINUX_EAGAIN, "empty receive does not block");
        t.Equal(m->Shutdown(pair[0], 1), 0, "shutdown writer");
        t.Equal(m->Recvfrom(pair[1], TestFixture::Buffer + 128, 8, 0x40, 0, 0), 0, "peer observes EOF");
    });
}
