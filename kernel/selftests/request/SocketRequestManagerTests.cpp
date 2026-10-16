#include "RequestTestSupport.hpp"

namespace request_selftest
{
static bool socket_pair(process_t* process, uint64_t base, uint32_t type, int32_t* pair)
{
    if (!expect(syscall_call(SYSCALL_socketpair, 1, type, 0, base) == 0, "socketpair fixture"))
    {
        return false;
    }

    fixture_load(process, base, pair, 8);

    return true;
}

static bool test_socket_creation(process_t* process, uint64_t base, uint64_t number)
{
    bool ok = expect(syscall_call(SYSCALL_socket, 2, 1, 0) == (uint64_t) -97, "socket networking family explicitly unsupported");

    ok &= expect(syscall_call(SYSCALL_socket, 1, 1, 1) == (uint64_t) -93 && syscall_call(SYSCALL_socket, 1, 3, 0) == (uint64_t) -94, "socket protocol and type errors");

    ok &= expect(syscall_call(SYSCALL_socket, 1, 0x10000001, 0) == LINUX_EINVAL, "socket rejects unknown creation flags");

    const size_t descriptors = descriptor_count(process);

    ok &= expect(syscall_call(SYSCALL_socketpair, 1, 1, 0, CANONICAL_KERNEL_BASE) == LINUX_EFAULT && descriptor_count(process) == descriptors,
        "socketpair output fault rolls back both descriptors");

    const uint64_t limit = process->fileLimit.current;

    process->fileLimit.current = 1;

    ok &= expect(syscall_call(SYSCALL_socketpair, 1, 1, 0, base) == LINUX_EMFILE && descriptor_count(process) == descriptors,
        "socketpair second descriptor exhaustion rolls back first");

    process->fileLimit.current = limit;

    const uint32_t types[] = {1U, 2U, 5U};

    for (uint32_t type : types)
    {
        int32_t pair[2];

        if (!socket_pair(process, base, type | 04000 | 02000000, pair))
        {
            return false;
        }

        ok &= expect(pair[0] != pair[1] && syscall_call(SYSCALL_fcntl, pair[0], 1) == 1 && syscall_call(SYSCALL_fcntl, pair[0], 3) == (2 | 04000),
            "socket flags separate descriptor CLOEXEC from file NONBLOCK");

        const char payload[] = "local";

        fixture_store(process, base + 128, payload, sizeof(payload));

        ok &= expect(syscall_call(SYSCALL_write, pair[0], base + 128, sizeof(payload)) == sizeof(payload) &&
                         syscall_call(SYSCALL_read, pair[1], base + 256, sizeof(payload)) == sizeof(payload),
            "socket file read/write transfers real payload");

        char received[sizeof(payload)] = {};

        fixture_load(process, base + 256, received, sizeof(received));

        ok &= expect(memcmp(payload, received, sizeof(payload)) == 0, "socket payload byte comparison");

        ok &= expect(syscall_call(SYSCALL_lseek, pair[0], 0, 0) == (uint64_t) -29, "socket is not seekable");

        if (type != 1)
        {
            ok &= expect(syscall_call(SYSCALL_sendto, pair[0], 0, 0, 0x4000) == 0 && syscall_call(SYSCALL_recvfrom, pair[1], 0, 0, 0) == 0 &&
                             syscall_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == (uint64_t) -11,
                "socket empty message is delivered and consumed");
        }

        ok &= cleanup(syscall_call(SYSCALL_close, pair[0]) == 0 && syscall_call(SYSCALL_close, pair[1]) == 0, "socketpair descriptor cleanup");
    }

    if (number == SYSCALL_socket)
    {
        const uint64_t fd = syscall_call(number, (1ULL << 32) | 1, (1ULL << 32) | 1, 0);

        ok &= expect((int64_t) fd >= 0, "socket decodes int arguments from low 32 bits");

        if ((int64_t) fd >= 0)
        {
            ok &= cleanup(syscall_call(SYSCALL_close, fd) == 0, "socket scalar descriptor cleanup");
        }
    }

    return ok;
}

static bool test_socket_connection(process_t* process, uint64_t base, uint64_t number)
{
    socket_address_t address = {};

    address.family = 1;

    memcpy(address.path + 1, "arx\0socket", 10);

    const uint32_t addressLength = 13;

    fixture_store(process, base + 128, &address, sizeof(address));

    const uint64_t server = syscall_call(SYSCALL_socket, 1, 1 | 04000 | 02000000, 0);

    const uint64_t client = syscall_call(SYSCALL_socket, 1, 1 | 04000, 0);

    if ((int64_t) server < 0 || (int64_t) client < 0)
    {
        if ((int64_t) server >= 0)
        {
            syscall_call(SYSCALL_close, server);
        }

        if ((int64_t) client >= 0)
        {
            syscall_call(SYSCALL_close, client);
        }

        return expect(false, "socket connection descriptors");
    }

    bool ok = expect(syscall_call(SYSCALL_getpeername, client, base + 256, base + 384) == (uint64_t) -107, "unconnected getpeername ENOTCONN");

    ok &= expect(syscall_call(SYSCALL_listen, server, 4) == LINUX_EINVAL, "listen requires a bound local address");

    ok &= expect(syscall_call(SYSCALL_bind, server, base + 128, addressLength) == 0, "socket abstract bind with embedded NUL");

    ok &= expect(syscall_call(SYSCALL_bind, server, base + 128, addressLength) == LINUX_EINVAL && syscall_call(SYSCALL_bind, client, base + 128, addressLength) == (uint64_t) -98,
        "bind duplicate socket/address errors");

    ok &= expect(syscall_call(SYSCALL_bind, client, CANONICAL_KERNEL_BASE, addressLength) == LINUX_EFAULT && syscall_call(SYSCALL_bind, client, base + 128, 1) == LINUX_EINVAL,
        "bind length and user pointer validation");

    ok &= expect(syscall_call(SYSCALL_listen, server, 4) == 0 && syscall_call(SYSCALL_accept, server) == (uint64_t) -11, "empty nonblocking accept EAGAIN");

    ok &= expect(syscall_call(SYSCALL_accept4, server, 0, 0, 1) == LINUX_EINVAL, "accept4 validates flags");

    if (number == SYSCALL_accept || number == SYSCALL_accept4)
    {
        syscall_call(SYSCALL_fcntl, server, 4, 2);

        syscall_peer_context_t context     = {nullptr, SYSCALL_connect, {client, base + 128, addressLength}, UINT64_MAX};
        uint64_t               accepted    = UINT64_MAX;
        const uint32_t         acceptFlags = number == SYSCALL_accept4 ? 04000 | 02000000 : 0;

        ok &= with_syscall_peer(process, &context, [&]()
        {
            accepted = syscall_call(number, server, 0, 0, acceptFlags);

            return expect((int64_t) accepted >= 0 && context.result == 0, "blocking accept wakes from peer connect");
        });

        if ((int64_t) accepted >= 0)
        {
            ok &= expect(syscall_call(SYSCALL_fcntl, accepted, 3) == (uint64_t) (2 | (acceptFlags & 04000)) &&
                             syscall_call(SYSCALL_fcntl, accepted, 1) == (uint64_t) ((acceptFlags & 02000000) != 0),
                "accept does not inherit listener flags; accept4 applies flags");

            syscall_call(SYSCALL_close, accepted);
        }
    }
    else
    {
        ok &= expect(syscall_call(SYSCALL_connect, client, base + 128, addressLength) == 0 && syscall_call(SYSCALL_connect, client, base + 128, addressLength) == (uint64_t) -106,
            "local stream connect and EISCONN");

        const uint32_t addressCapacity = sizeof(socket_address_t);

        fixture_store(process, base + 384, &addressCapacity, sizeof(addressCapacity));

        ok &= expect(syscall_call(SYSCALL_accept4, server, CANONICAL_KERNEL_BASE, base + 384, 0) == LINUX_EFAULT, "accept output fault preserves pending connection");

        const uint64_t accepted = syscall_call(SYSCALL_accept4, server, 0, 0, 04000);

        ok &= expect((int64_t) accepted >= 0, "queued connection accepted");

        if ((int64_t) accepted >= 0)
        {
            const uint32_t capacity = sizeof(socket_address_t);

            fixture_store(process, base + 384, &capacity, 4);

            ok &= expect(syscall_call(SYSCALL_getpeername, client, base + 256, base + 384) == 0, "connected peer name");

            socket_address_t queried = {};
            uint32_t         actual  = 0;

            fixture_load(process, base + 256, &queried, sizeof(queried));

            fixture_load(process, base + 384, &actual, 4);

            ok &= expect(actual == addressLength && memcmp(&queried, &address, addressLength) == 0, "abstract name preserves exact length and embedded zeros");

            const uint32_t shortLength = 1;

            fixture_store(process, base + 384, &shortLength, 4);

            ok &= expect(syscall_call(SYSCALL_getsockname, server, base + 256, base + 384) == 0, "socket name truncates to supplied capacity");

            fixture_load(process, base + 384, &actual, 4);

            ok &= expect(actual == addressLength, "socket name reports full size after truncation");

            syscall_call(SYSCALL_close, accepted);
        }
    }

    if (number == SYSCALL_listen)
    {
        syscall_call(SYSCALL_fcntl, server, 4, 2);

        syscall_peer_context_t context = {nullptr, SYSCALL_shutdown, {server, 0}, UINT64_MAX};

        ok &= with_syscall_peer(process, &context,
            [&]()
        {
            return expect(syscall_call(SYSCALL_accept, server) == LINUX_EINVAL && context.result == 0, "listener shutdown wakes blocked accept");
        });
    }

    const uint32_t faultCapacity = sizeof(socket_address_t);

    fixture_store(process, base + 384, &faultCapacity, sizeof(faultCapacity));

    ok &= expect(syscall_call(SYSCALL_getsockname, server, CANONICAL_KERNEL_BASE, base + 384) == LINUX_EFAULT, "socket name rejects kernel output");

    syscall_call(SYSCALL_close, server);

    syscall_call(SYSCALL_close, client);

    const uint64_t rebound = syscall_call(SYSCALL_socket, 1, 1, 0);

    ok &= expect((int64_t) rebound >= 0 && syscall_call(SYSCALL_bind, rebound, base + 128, addressLength) == 0, "last close releases abstract name");

    if ((int64_t) rebound >= 0)
    {
        syscall_call(SYSCALL_close, rebound);
    }

    return ok;
}

static bool test_socket_message(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 2 | 04000, pair))
    {
        return false;
    }

    const char payload[] = "abcdef";

    fixture_store(process, base + 128, payload, 6);

    file_iovec_t vectors[2] = {{base + 128, 2}, {base + 130, 4}};

    fixture_store(process, base + 256, vectors, sizeof(vectors));

    socket_message_t message = {};

    message.vectors     = base + 256;
    message.vectorCount = 2;

    const uint64_t header = base + PAGE_SIZE - 24;

    fixture_store(process, header, &message, sizeof(message));

    bool ok = expect(syscall_call(SYSCALL_sendmsg, pair[0], header, 0) == 6, "sendmsg decodes LP64 header across pages and gathers iovecs");

    vectors[0] = {base + 512, 1};
    vectors[1] = {base + 768, 2};

    fixture_store(process, base + 256, vectors, sizeof(vectors));

    message.controlLength = 17;
    message.flags         = UINT32_MAX;

    fixture_store(process, header, &message, sizeof(message));

    ok &= expect(syscall_call(SYSCALL_recvmsg, pair[1], header, 0x20) == 6, "recvmsg MSG_TRUNC returns full datagram length");

    fixture_load(process, header, &message, sizeof(message));

    char first = 0, tail[2] = {};

    fixture_load(process, base + 512, &first, 1);

    fixture_load(process, base + 768, tail, 2);

    ok &= expect(first == 'a' && tail[0] == 'b' && tail[1] == 'c' && message.flags == 0x20 && message.controlLength == 0 && message.nameLength == 0 && message.vectorCount == 2,
        "recvmsg scatter data, truncation flag and ancillary length outputs");

    message.vectorCount = 1025;

    fixture_store(process, header, &message, sizeof(message));

    ok &= expect(syscall_call(SYSCALL_sendmsg, pair[0], header, 0) == (uint64_t) -90, "message iovec count bound");

    message.vectorCount   = 1;
    message.controlLength = 1;

    fixture_store(process, header, &message, sizeof(message));

    ok &= expect(syscall_call(SYSCALL_sendmsg, pair[0], header, 0) == LINUX_EOPNOTSUPP, "ancillary send explicitly unsupported");

    message.controlLength = 0;
    message.vectors       = CANONICAL_KERNEL_BASE;

    fixture_store(process, header, &message, sizeof(message));

    ok &= expect(syscall_call(SYSCALL_sendmsg, pair[0], header, 0) == LINUX_EFAULT && syscall_call(SYSCALL_recvmsg, pair[1], CANONICAL_KERNEL_BASE, 0) == LINUX_EFAULT,
        "message validates nested and top-level pointers");

    vectors[0] = {base + 128, UINT64_MAX};

    fixture_store(process, base + 256, vectors, sizeof(vectors));

    message.vectors = base + 256;

    fixture_store(process, header, &message, sizeof(message));

    ok &= expect(syscall_call(SYSCALL_sendmsg, pair[0], header, 0) == LINUX_EINVAL, "message rejects overflowing vector lengths");

    syscall_call(SYSCALL_close, pair[0]);

    syscall_call(SYSCALL_close, pair[1]);

    return ok;
}

static bool test_socket_transfer(process_t* process, uint64_t base, uint64_t number)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1, pair))
    {
        return false;
    }

    const char data[] = "abcdef";

    fixture_store(process, base + 128, data, 6);

    bool ok = expect(syscall_call(SYSCALL_recvfrom, pair[1], base + 256, 6, 0x40) == (uint64_t) -11, "per-call MSG_DONTWAIT returns EAGAIN");

    syscall_peer_context_t context = {nullptr, SYSCALL_sendto, {(uint64_t) pair[0], base + 128, 6, 0x4000}, UINT64_MAX};

    ok &= with_syscall_peer(process, &context, [&]()
    {
        return expect(syscall_call(SYSCALL_recvfrom, pair[1], base + 256, 6, 0) == 6 && context.result == 6, "blocking socket receive wakes on real peer send");
    });
    char received[6] = {};

    fixture_load(process, base + 256, received, 6);

    ok &= expect(memcmp(received, data, 6) == 0, "blocking receive payload matches");

    ok &= expect(syscall_call(SYSCALL_sendto, pair[0], base + 128, 2, 0x4000) == 2 && syscall_call(SYSCALL_sendto, pair[0], base + 130, 4, 0x4000) == 4,
        "stream sends enqueue distinct chunks");

    ok &= expect(syscall_call(SYSCALL_recvfrom, pair[1], base + 256, 2, 2) == 2, "MSG_PEEK leaves bytes queued");

    ok &= expect(syscall_call(SYSCALL_recvfrom, pair[1], CANONICAL_KERNEL_BASE, 2, 0) == LINUX_EFAULT && syscall_call(SYSCALL_recvfrom, pair[1], base + 256, 6, 0x100) == 6,
        "receive fault preserves data; WAITALL joins stream chunks");

    fixture_load(process, base + 256, received, 6);

    ok &= expect(memcmp(received, data, 6) == 0, "stream chunk order preserved");

    file_iovec_t vectors[2] = {{base + 128, 2}, {base + 130, 4}};

    fixture_store(process, base + 512, vectors, sizeof(vectors));

    ok &= expect(syscall_call(SYSCALL_writev, pair[0], base + 512, 2) == 6 && syscall_call(SYSCALL_read, pair[1], base + 256, 6) == 6,
        "socket writev integrates with VFS descriptor callbacks");

    ok &= expect(syscall_call(SYSCALL_write, pair[0], base + 128, 6) == 6, "socket ioctl queue fixture");

    ok &= expect(syscall_call(SYSCALL_ioctl, pair[1], 0x541b, base + 1024) == 0, "socket FIONREAD ioctl");

    int32_t available = 0;

    fixture_load(process, base + 1024, &available, sizeof(available));

    ok &= expect(available == 6, "FIONREAD reports queued stream bytes");

    syscall_call(SYSCALL_mprotect, base + 3 * PAGE_SIZE, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_recvfrom, pair[1], base + 3 * PAGE_SIZE, 6, 0) == LINUX_EFAULT, "socket receive rejects readonly output without dequeue");

    ok &= expect(syscall_call(SYSCALL_read, pair[1], base + 256, 6) == 6, "readonly output fault preserves queued socket payload");

    ok &= expect(syscall_call(SYSCALL_sendto, pair[0], CANONICAL_KERNEL_BASE, 1, 0x4000) == LINUX_EFAULT &&
                     syscall_call(SYSCALL_sendto, pair[0], base + 128, 1, 1) == LINUX_EOPNOTSUPP,
        "send rejects invalid buffers and unsupported flags");

    syscall_call(SYSCALL_close, pair[0]);

    syscall_call(SYSCALL_close, pair[1]);

    if (number == SYSCALL_sendto || number == SYSCALL_recvfrom)
    {
        int32_t datagrams[2];

        if (!socket_pair(process, base, 2 | 04000, datagrams))
        {
            return false;
        }

        ok &= expect(syscall_call(SYSCALL_sendto, datagrams[0], base + 128, 65537, 0) == (uint64_t) -90, "oversized datagram fails atomically before buffer access");

        ok &= expect(syscall_call(SYSCALL_sendto, datagrams[0], base + 128, 6, 0) == 6 && syscall_call(SYSCALL_recvfrom, datagrams[1], base + 256, 2, 0x20) == 6,
            "datagram truncation discards tail and returns full size");

        ok &= expect(syscall_call(SYSCALL_recvfrom, datagrams[1], base + 256, 6, 0) == (uint64_t) -11, "truncated datagram is consumed once");

        syscall_call(SYSCALL_close, datagrams[0]);

        syscall_call(SYSCALL_close, datagrams[1]);
    }

    return ok;
}

static bool test_socket_shutdown(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1 | 04000, pair))
    {
        return false;
    }

    const uint8_t byte = 0x5a;

    fixture_store(process, base + 128, &byte, 1);

    bool ok = expect(syscall_call(SYSCALL_shutdown, pair[0], 3) == LINUX_EINVAL, "shutdown validates direction");

    ok &= expect(syscall_call(SYSCALL_sendto, pair[0], base + 128, 1, 0x4000) == 1 && syscall_call(SYSCALL_shutdown, pair[0], 1) == 0, "shutdown write half after queued data");

    ok &= expect(syscall_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == 1 && syscall_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == 0,
        "shutdown drains queued payload before EOF");

    ok &= expect(syscall_call(SYSCALL_sendto, pair[1], base + 128, 1, 0x4000) == 1 && syscall_call(SYSCALL_read, pair[0], base + 256, 1) == 1,
        "half-close preserves reverse direction");

    ok &= expect(syscall_call(SYSCALL_sendto, pair[0], base + 128, 1, 0x4000) == (uint64_t) -32, "MSG_NOSIGNAL returns EPIPE after write shutdown");

    auto           resources = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps();
    task_t*        task      = resources->taskManager->GetCurrentTask();
    const uint64_t pending   = task->signalPending;

    ok &= expect(syscall_call(SYSCALL_sendto, pair[0], base + 128, 1, 0) == (uint64_t) -32 && (task->signalPending & (1ULL << 12)) != 0, "broken socket queues thread SIGPIPE");

    task->signalPending = pending;

    event_pollfd_t descriptor = {pair[1], 0x2001, 0};

    fixture_store(process, base + 512, &descriptor, sizeof(descriptor));

    ok &= expect(syscall_call(SYSCALL_poll, base + 512, 1, 0) == 1, "socket poll observes read half closure");

    fixture_load(process, base + 512, &descriptor, sizeof(descriptor));

    ok &= expect((descriptor.revents & 0x2001) == 0x2001, "socket POLLIN/POLLRDHUP bits");

    syscall_call(SYSCALL_close, pair[0]);

    ok &= expect(syscall_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == 0, "peer last close produces EOF");

    syscall_call(SYSCALL_close, pair[1]);

    return ok;
}

static bool test_socket_options(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1, pair))
    {
        return false;
    }

    uint32_t length = 4;

    fixture_store(process, base + 128, &length, 4);

    bool ok = expect(syscall_call(SYSCALL_getsockopt, pair[0], 1, 3, base + 256, base + 128) == 0, "getsockopt SO_TYPE");

    uint32_t value = 0;

    fixture_load(process, base + 256, &value, 4);

    ok &= expect(value == 1, "SO_TYPE reports base type without flags");

    value = 8192;

    fixture_store(process, base + 256, &value, 4);

    ok &= expect(syscall_call(SYSCALL_setsockopt, pair[0], 1, 7, base + 256, 4) == 0 && syscall_call(SYSCALL_getsockopt, pair[0], 1, 7, base + 256, base + 128) == 0,
        "socket buffer option round trip");

    fixture_load(process, base + 256, &value, 4);

    ok &= expect(value == 16384, "Linux SO_SNDBUF doubles supplied value");

    length = 12;

    fixture_store(process, base + 128, &length, 4);

    ok &= expect(syscall_call(SYSCALL_getsockopt, pair[0], 1, 17, base + 256, base + 128) == 0, "SO_PEERCRED wire output");

    int32_t credentials[3] = {};

    fixture_load(process, base + 256, credentials, sizeof(credentials));

    ok &= expect(credentials[0] == (int32_t) process->id && credentials[1] == (int32_t) process->credentials.user.effective &&
                     credentials[2] == (int32_t) process->credentials.group.effective,
        "socketpair credentials snapshot");

    ok &= expect(syscall_call(SYSCALL_setsockopt, pair[0], 1, 7, base + 256, 3) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_setsockopt, pair[0], 1, 7, CANONICAL_KERNEL_BASE, 4) == LINUX_EFAULT,
        "setsockopt length and pointer validation");

    ok &= expect(syscall_call(SYSCALL_getsockopt, pair[0], 1, 999, base + 256, base + 128) == (uint64_t) -92 &&
                     syscall_call(SYSCALL_setsockopt, pair[0], 999, 7, base + 256, 4) == (uint64_t) -92,
        "unknown socket options return ENOPROTOOPT");

    ok &= expect(syscall_call(SYSCALL_getsockopt, pair[0], 1, 3, base + 256, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "getsockopt validates socklen pointer");

    length = 0;

    fixture_store(process, base + 128, &length, 4);

    ok &= expect(syscall_call(SYSCALL_getsockopt, pair[0], 1, 3, 0, base + 128) == 0, "zero-length option query ignores value pointer");

    syscall_call(SYSCALL_close, pair[0]);

    syscall_call(SYSCALL_close, pair[1]);

    return ok;
}

static bool test_socket_waits(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1, pair))
    {
        return false;
    }

    bool ok = true;

    for (size_t i = 0; i < 16; ++i)
    {
        ok &= expect(syscall_call(SYSCALL_sendto, pair[0], base + 128, PAGE_SIZE, 0x4040) == PAGE_SIZE, "fill bounded stream receive queue");
    }

    ok &= expect(syscall_call(SYSCALL_sendto, pair[0], base + 128, 1, 0x4040) == (uint64_t) -11, "full socket queue returns EAGAIN without dropping data");

    syscall_peer_context_t context = {nullptr, SYSCALL_recvfrom, {(uint64_t) pair[1], base + 256, PAGE_SIZE, 0}, UINT64_MAX};

    ok &= with_syscall_peer(process, &context, [&]()
    {
        return expect(syscall_call(SYSCALL_sendto, pair[0], base + 128, 1, 0x4000) == 1 && context.result == PAGE_SIZE, "blocking send wakes after peer drains queue");
    });
    syscall_call(SYSCALL_close, pair[0]);

    syscall_call(SYSCALL_close, pair[1]);

    if (!socket_pair(process, base, 1, pair))
    {
        return false;
    }

    signal_action_t action = {base + 128, 0x04000000, base + 768, 0};

    fixture_store(process, base + 1024, &action, sizeof(action));

    ok &= expect(syscall_call(SYSCALL_rt_sigaction, 10, base + 1024, 0, 8) == 0, "socket interruption signal fixture");

    context = {nullptr, SYSCALL_kill, {process->id, 10}, UINT64_MAX};

    ok &= with_syscall_peer(process, &context, [&]()
    {
        return expect(syscall_call(SYSCALL_recvfrom, pair[1], base + 256, 1, 0) == (uint64_t) -4 && context.result == 0, "socket wait returns EINTR for real peer signal");
    });
    __atomic_store_n(&process->signalPending, 0ULL, __ATOMIC_RELEASE);

    syscall_call(SYSCALL_close, pair[0]);

    syscall_call(SYSCALL_close, pair[1]);

    return ok;
}

static bool test_socket_datagram_names(process_t* process, uint64_t base)
{
    const uint64_t sender = syscall_call(SYSCALL_socket, 1, 2 | 04000, 0);

    const uint64_t receiver = syscall_call(SYSCALL_socket, 1, 2 | 04000, 0);

    if ((int64_t) sender < 0 || (int64_t) receiver < 0)
    {
        if ((int64_t) sender >= 0)
        {
            syscall_call(SYSCALL_close, sender);
        }

        if ((int64_t) receiver >= 0)
        {
            syscall_call(SYSCALL_close, receiver);
        }

        return expect(false, "named datagram fixture descriptors");
    }

    socket_address_t address = {};

    address.family = 1;

    memcpy(address.path + 1, "arx-recv", 8);

    const uint32_t length = 11;

    fixture_store(process, base + 128, &address, sizeof(address));

    address.path[1] = 'b';

    fixture_store(process, base + 256, &address, sizeof(address));

    bool ok = expect(syscall_call(SYSCALL_bind, receiver, base + 128, length) == 0 && syscall_call(SYSCALL_bind, sender, base + 256, length) == 0, "named datagram bind");

    const uint8_t payload = 0x93;

    fixture_store(process, base + 512, &payload, 1);

    const uint32_t capacity = sizeof(address);

    fixture_store(process, base + 640, &capacity, 4);

    ok &= expect(syscall_call(SYSCALL_sendto, sender, base + 512, 1, 0, base + 128, length) == 1 &&
                     syscall_call(SYSCALL_recvfrom, receiver, base + 768, 1, 0, base + 896, base + 640) == 1,
        "named datagram sendto/recvfrom");

    socket_address_t source       = {};
    uint32_t         actualLength = 0;

    fixture_load(process, base + 896, &source, sizeof(source));

    fixture_load(process, base + 640, &actualLength, 4);

    ok &= expect(actualLength == length && memcmp(&source, &address, length) == 0, "recvfrom reports sender abstract address");

    ok &= expect(syscall_call(SYSCALL_connect, sender, base + 128, length) == 0 && syscall_call(SYSCALL_sendto, sender, base + 512, 1, 0) == 1 &&
                     syscall_call(SYSCALL_read, receiver, base + 768, 1) == 1,
        "connected datagram sends without destination");

    address = {};

    fixture_store(process, base + 1024, &address, sizeof(address));

    ok &= expect(syscall_call(SYSCALL_connect, sender, base + 1024, 2) == 0 && syscall_call(SYSCALL_sendto, sender, base + 512, 1, 0) == (uint64_t) -89,
        "AF_UNSPEC disconnects datagram default peer");

    address.family = 1;

    memcpy(address.path, "/tmp/socket", 12);

    fixture_store(process, base + 1024, &address, sizeof(address));

    ok &= expect(syscall_call(SYSCALL_bind, sender, base + 1024, 14) == LINUX_EOPNOTSUPP, "filesystem socket paths explicitly require future VFS backend");

    syscall_call(SYSCALL_close, sender);

    syscall_call(SYSCALL_close, receiver);

    return ok;
}

static bool test_socket_epoll(process_t* process, uint64_t base)
{
    int32_t pair[2];

    if (!socket_pair(process, base, 1 | 04000, pair))
    {
        return false;
    }

    const uint64_t epoll = syscall_call(SYSCALL_epoll_create1, 0);

    event_epoll_event_t watch = {0x80000001U, 0x1122334455667788ULL};

    fixture_store(process, base + 128, &watch, sizeof(watch));

    bool ok = expect((int64_t) epoll >= 0 && syscall_call(SYSCALL_epoll_ctl, epoll, 1, pair[1], base + 128) == 0, "socket edge-triggered epoll registration");

    ok &= expect(syscall_call(SYSCALL_epoll_pwait, epoll, base + 256, 1, 0, 0, 8) == 0, "empty socket has no readable epoll event");

    const uint8_t data = 0x73;

    fixture_store(process, base + 512, &data, 1);

    ok &= expect(syscall_call(SYSCALL_write, pair[0], base + 512, 1) == 1 && syscall_call(SYSCALL_epoll_pwait, epoll, base + 256, 1, 0, 0, 8) == 1,
        "socket producer wakes edge-triggered epoll");

    event_epoll_event_t result = {};

    fixture_load(process, base + 256, &result, sizeof(result));

    ok &= expect((result.events & 1) != 0 && result.data == watch.data && syscall_call(SYSCALL_epoll_pwait, epoll, base + 256, 1, 0, 0, 8) == 0,
        "socket epoll event preserves user data and is consumed once");

    ok &= expect(syscall_call(SYSCALL_read, pair[1], base + 512, 1) == 1 && syscall_call(SYSCALL_write, pair[0], base + 512, 1) == 1 &&
                     syscall_call(SYSCALL_epoll_pwait, epoll, base + 256, 1, 0, 0, 8) == 1,
        "drain then refill produces another socket edge");

    const uint64_t alias = syscall_call(SYSCALL_dup, pair[0]);

    syscall_call(SYSCALL_close, pair[0]);

    ok &= expect((int64_t) alias >= 0 && syscall_call(SYSCALL_write, alias, base + 512, 1) == 1, "dup retains socket endpoint after original descriptor close");

    if ((int64_t) alias >= 0)
    {
        syscall_call(SYSCALL_close, alias);
    }

    syscall_call(SYSCALL_close, pair[1]);

    if ((int64_t) epoll >= 0)
    {
        syscall_call(SYSCALL_close, epoll);
    }

    return ok;
}

template <uint64_t Number>
static bool validate_socket(const syscall_case_t*)
{
    return with_process([](process_t* process)
    {
        const uint64_t base = fixture_map(4);

        if ((int64_t) base < 0)
        {
            return false;
        }

        bool ok;

        if (Number == SYSCALL_socket || Number == SYSCALL_socketpair)
        {
            ok = test_socket_creation(process, base, Number);
        }
        else if (Number == SYSCALL_bind || Number == SYSCALL_connect || Number == SYSCALL_listen || Number == SYSCALL_accept || Number == SYSCALL_accept4 ||
                 Number == SYSCALL_getsockname || Number == SYSCALL_getpeername)
        {
            ok = test_socket_connection(process, base, Number);
        }
        else if (Number == SYSCALL_sendmsg || Number == SYSCALL_recvmsg)
        {
            ok = test_socket_message(process, base);
        }
        else if (Number == SYSCALL_shutdown)
        {
            ok = test_socket_shutdown(process, base);
        }
        else if (Number == SYSCALL_getsockopt || Number == SYSCALL_setsockopt)
        {
            ok = test_socket_options(process, base);
        }
        else
        {
            ok = test_socket_transfer(process, base, Number);
        }

        if (Number == SYSCALL_sendto)
        {
            ok &= test_socket_waits(process, base);

            ok &= test_socket_datagram_names(process, base);

            ok &= test_socket_epoll(process, base);
        }

        const uint64_t nonsocket = syscall_call(SYSCALL_eventfd2, 0, 04000);

        if ((int64_t) nonsocket >= 0)
        {
            if (Number != SYSCALL_socket && Number != SYSCALL_socketpair)
            {
                ok &= expect(syscall_call(Number, nonsocket, 0, 0, 0, 0, 0) == (uint64_t) -88, "socket operation rejects nonsocket descriptor before user copies");
            }

            syscall_call(SYSCALL_close, nonsocket);
        }

        ok &= expect(descriptor_count(process) == 0, "socket syscall fixture releases all descriptors");

        ok &= expect(syscall_call(Number, UINT32_MAX, 0, 0, 0, 0, 0) == (Number == SYSCALL_socket || Number == SYSCALL_socketpair ? (uint64_t) -97 : LINUX_EBADF),
            "socket invalid descriptor/domain ABI validation");

        return ok;
    });
}

void test_socket_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<SocketRequestManager> tests[] = {
        {"HandleSocketRequest", &SocketRequestManager::HandleSocketRequest, SYSCALL_socket, validate_socket<SYSCALL_socket>},
        {"HandleSocketpairRequest", &SocketRequestManager::HandleSocketpairRequest, SYSCALL_socketpair, validate_socket<SYSCALL_socketpair>},
        {"HandleBindRequest", &SocketRequestManager::HandleBindRequest, SYSCALL_bind, validate_socket<SYSCALL_bind>},
        {"HandleConnectRequest", &SocketRequestManager::HandleConnectRequest, SYSCALL_connect, validate_socket<SYSCALL_connect>},
        {"HandleListenRequest", &SocketRequestManager::HandleListenRequest, SYSCALL_listen, validate_socket<SYSCALL_listen>},
        {"HandleAccept4Request", &SocketRequestManager::HandleAccept4Request, SYSCALL_accept4, validate_socket<SYSCALL_accept4>},
        {"HandleAcceptRequest", &SocketRequestManager::HandleAcceptRequest, SYSCALL_accept, validate_socket<SYSCALL_accept>},
        {"HandleRecvfromRequest", &SocketRequestManager::HandleRecvfromRequest, SYSCALL_recvfrom, validate_socket<SYSCALL_recvfrom>},
        {"HandleRecvmsgRequest", &SocketRequestManager::HandleRecvmsgRequest, SYSCALL_recvmsg, validate_socket<SYSCALL_recvmsg>},
        {"HandleSendtoRequest", &SocketRequestManager::HandleSendtoRequest, SYSCALL_sendto, validate_socket<SYSCALL_sendto>},
        {"HandleSendmsgRequest", &SocketRequestManager::HandleSendmsgRequest, SYSCALL_sendmsg, validate_socket<SYSCALL_sendmsg>},
        {"HandleShutdownRequest", &SocketRequestManager::HandleShutdownRequest, SYSCALL_shutdown, validate_socket<SYSCALL_shutdown>},
        {"HandleGetsockoptRequest", &SocketRequestManager::HandleGetsockoptRequest, SYSCALL_getsockopt, validate_socket<SYSCALL_getsockopt>},
        {"HandleSetsockoptRequest", &SocketRequestManager::HandleSetsockoptRequest, SYSCALL_setsockopt, validate_socket<SYSCALL_setsockopt>},
        {"HandleGetsocknameRequest", &SocketRequestManager::HandleGetsocknameRequest, SYSCALL_getsockname, validate_socket<SYSCALL_getsockname>},
        {"HandleGetpeernameRequest", &SocketRequestManager::HandleGetpeernameRequest, SYSCALL_getpeername, validate_socket<SYSCALL_getpeername>},
    };

    SocketRequestManager unavailable;

    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};

        if ((unavailable.*test.method)(&frame) != LINUX_ENOSYS || (caps->socketRequestManager != nullptr && (caps->socketRequestManager->*test.method)(nullptr) != LINUX_EINVAL))
        {
            expect(false, "socket missing dependency/null-frame validation");
        }
    }

    run_syscalls(caps->socketRequestManager, tests);
}

} // namespace request_selftest
