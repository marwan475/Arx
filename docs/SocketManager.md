# Local sockets and the socket syscall interface

`SocketRequestManager` implements the 16 Linux socket syscall entry points through
`LogicLayerCaps::socketManager`. Request handlers decode register arguments as
Linux integers, pointers, sizes and 32-bit `socklen_t` values. `SocketManager`
owns socket state and local transport behavior. Future networking belongs behind
this interface; no NIC driver, IP, TCP, UDP or other network transport is included.

## Supported local operations

- `AF_UNIX`, protocol zero, with `SOCK_STREAM`, `SOCK_DGRAM` and
  `SOCK_SEQPACKET`; `SOCK_NONBLOCK` and `SOCK_CLOEXEC` creation flags.
- Anonymous connected `socketpair` endpoints and explicitly bound Linux abstract
  addresses. Abstract names retain their exact byte length, including embedded
  NULs, and disappear when the last endpoint reference closes.
- `bind`, `listen`, `connect`, `accept` and `accept4`, with pending connection
  queues and blocking or nonblocking waits. Accepted descriptors receive only
  the flags requested by `accept4`.
- `getsockname` and `getpeername`, including truncated output buffers and full
  returned lengths. Datagram `connect(AF_UNSPEC)` clears the default destination.
- `sendto`, `recvfrom`, `sendmsg` and `recvmsg`; LP64 `msghdr` and `iovec` layouts;
  scatter/gather transfer; message boundaries and truncation; `MSG_DONTWAIT`,
  `MSG_PEEK`, `MSG_TRUNC`, stream `MSG_WAITALL`, and `MSG_NOSIGNAL`.
- `shutdown`, queued-data draining before stream EOF, peer-close notification and
  thread-directed `SIGPIPE` for broken writes without `MSG_NOSIGNAL`.
- Descriptor `read`, `write`, `readv`, `writev`, `close`, `dup`, `fcntl`,
  `FIONREAD`, socket file type in `fstat`, and existing poll/select/epoll support.
- `SOL_SOCKET` queries for `SO_TYPE`, `SO_ERROR`, `SO_SNDBUF`, `SO_RCVBUF`,
  `SO_PEERCRED`, `SO_ACCEPTCONN`, `SO_PROTOCOL` and `SO_DOMAIN`. Buffer sizes can
  be set; reported sizes use Linux's doubling and minimum-size convention.

User copies use the existing process-memory access guards. Blocking paths retain
file references, snapshot the event sequence before checking socket state, and
release descriptor, address-space and socket locks before sleeping. One socket
state lock protects endpoint links and message queues across CPUs. Close removes
weak peer links before freeing endpoint state. Readiness notifications are
published outside the state lock and track rising readiness bits for epoll edges.

The current bounded implementation allows 128 live endpoints, 128 queued records
per endpoint, up to 64 KiB of queued payload, and up to 1024 message iovecs. These
are kernel resource limits, not Linux-wide constants. Buffer accounting is local
and bounded; it does not reproduce Linux's allocator-specific accounting.

## Features still needed

Unsupported features return explicit Linux errors instead of pretending to work:

- Network families return `EAFNOSUPPORT`. Future networking needs a transport
  backend, network interfaces/drivers, packet queues, routing/address management,
  and protocol implementations. The syscall interface need not change.
- Filesystem socket addresses and autobind return `EOPNOTSUPP`. Pathname sockets
  need VFS socket-node creation, path resolution, ownership/permissions, unlink
  semantics, and a persistent association between the node and bound endpoint.
- Sending ancillary data returns `EOPNOTSUPP`. `SCM_RIGHTS` needs transactional
  cross-process descriptor installation, retained file ownership while queued,
  truncation cleanup and garbage collection for descriptor-reference cycles.
  `SCM_CREDENTIALS` needs credential-message validation and delivery policy.
- Socket timeouts, linger, credential passing, out-of-band data, error queues,
  transport-specific options, and the combined `MSG_PEEK | MSG_WAITALL` operation
  are not implemented. Unsupported options return `ENOPROTOOPT`; unsupported
  message flags return `EOPNOTSUPP`. No timeout or ancillary data is silently
  accepted. `recvmsg` clears unused ancillary output length.

## Verification and ABI references

`kernel/selftests/syscalltests.cpp` invokes the real dispatcher for every socket
entry point. Tests cover actual payloads, blocking peer tasks, queue saturation,
EINTR, flags, names, datagram truncation, raw message layouts, user-memory faults,
half-close, SIGPIPE, dup, epoll edges, and descriptor rollback/cleanup. Each
request handler is also checked for missing dependencies and null frames.

The ABI behavior follows the Linux man-pages descriptions for
[unix(7)](https://man7.org/linux/man-pages/man7/unix.7.html),
[socket(7)](https://man7.org/linux/man-pages/man7/socket.7.html),
[accept(2)](https://man7.org/linux/man-pages/man2/accept.2.html),
[send(2)](https://man7.org/linux/man-pages/man2/send.2.html), and
[recv(2)](https://man7.org/linux/man-pages/man2/recv.2.html).
