# Add pipe, memory-file and UNIX socket debug views

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Expose IPC object state and ownership relationships so an investigator can
explain blocked reads/writes/connect/accept, missing pipe endpoints, socket queue
pressure and retained passed descriptors. Current commands can reveal some file
paths but do not decode these anonymous-file object graphs.

Follow [the workflow](../TICKET.MD), required grilling and repository skills.
Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Depends on [foundations](build_debug_framework_foundation.md) and
[task-aware descriptors](add_debug_process_resource_views.md). Reuse backing-page
identities from [memory views](add_debug_virtual_memory_views.md) and readiness
relationships from [signal/event views](add_debug_signal_event_views.md), without
a circular requirement that every adapter be implemented first. Shared record
types/helpers are owned by the relevant foundation/provider ticket. Reference:
[assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment). This ticket starts open;
it does not authorize implementation.

### Scope and internal milestones

1. Add IPC/SocketManager-owned debug headers and agreed maintained operation,
   failure, bytes/queue and current-object stats. Define successful versus short/
   failed transfers and whether values count calls, bytes or live objects.
   Register production manager records; tracking compiles out in release.
2. Discover pipe and memory-file objects through typed descriptor/file roots and
   verified operation-table identity before interpreting `privateData`. IPC has
   no existing global object catalogue. State that reachability limit; adding
   detached-object enumeration requires an explicit analysis decision and safe
   catalogue lifetime, not an assumed global list.
3. Show pipes' reader/writer file identity, references, ring capacity/head/size,
   packet-boundary metadata and stored readiness transitions. Show memory-file
   seals, inode/file size, backing-page slots and mapping relationships. Payload
   bytes are opt-in, bounded detail and not a default 64-KiB scan.
4. Expose SocketManager's endpoint list/count; decode UNIX addresses/type/state,
   peer links, pending accept chains/backlog/count, message chains/size/offset,
   queued bytes/limits, shutdown flags, credentials and stored readiness.
   Preserve embedded-NUL/abstract address semantics instead of treating every
   address as a C path string. No TCP/IP transport is implemented.
5. Show ancillary descriptor rights and credentials, linking each retained file
   to known process/task descriptors and socket message ownership. Deduplicate
   observed objects, label weak/owning/in-flight relationships and cap graph
   depth/edges. Do not claim a complete leak/garbage-collection proof.
6. Add bounded adapters and shared identity references, document roots/types,
   operation validation, ring ordering, lengths/lifetimes and future-native-reader
   requirements, and update `docs/debugging/GdbCommands.md`.

### Proposed interface and illustrative output

```text
arx-debug ipc pipes --tid 12 --limit 20
  fd=3 reader=<file> writer=<file> used=4096 capacity=65536 readiness=IN
arx-debug ipc memory-file --tid 12 --fd 4
  size=8192 seals=GROW pages=2 backing=<addr>
arx-debug sockets endpoints --limit 32
  endpoint=<addr> type=STREAM listening=true pending=1 backlog=8
arx-debug sockets messages <endpoint> --limit 8
  packet=0 bytes=128 offset=0 rights=[<file>,<file>] credentials=<values>
```

Settle grammar in analysis. Null/mismatched private data, partial publication,
invalid indices/counts, asymmetric links and cycles produce explicit bounded
results. Do not invoke Poll/Release/Read/Write or acquire target locks. No Request
providers, historical packet tracing, payload capture service or dump mechanics.

### Sources and unknowns

- [InterProcessCommunicationManager.cpp](../../kernel/layers/Logic/InterProcessCommunicationManager.cpp):
  `pipe_state_t`, packet map, `memory_file_t`, pipe/memory operations tables.
- [SocketManager.hpp](../../kernel/layers/Logic/SocketManager.hpp),
  [implementation](../../kernel/layers/Logic/SocketManager.cpp): `Endpoints`,
  `socket_endpoint_t`, `socket_packet_t`, `socket_ancillary_t` and queue limits.
- [VirtualFileSystem.hpp](../../kernel/layers/Logic/VirtualFileSystem.hpp):
  `file_t`, operations/private data, reference/observer lifetime.
- [ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp),
  [TaskManager.hpp](../../kernel/layers/Resource/TaskManager.hpp): descriptor owners.
- Unknowns: private/anonymous type exposure, operation identity description for a
  native reader, endpoint/packet limits, dropped/failed transfer counter semantics
  and whether a debug catalogue is warranted. Keep this workload read-only.

## Acceptance criteria

- [ ] Debug-only stats and collection roots expose IPC and UNIX socket state with clear transfer/count units and no release overhead.
- [ ] Pipe/memory-file decoding verifies type/operations first and reports descriptor-reachable scope accurately.
- [ ] Pipe rings/packets, memory seals/pages and socket peers/accept/message queues are bounded, correctly ordered and correlated with file owners.
- [ ] Ancillary rights and credentials are inspectable without unsupported complete-reference/leak claims or automatic payload scans.
- [ ] Invalid types, counts, links, missing endpoints and partial groups return explicit bounded status without target calls/locks.
- [ ] Native-reader semantics, shared identities and command-guide examples cover every selected rich collection; Request/dump/history work is absent.

## Verification approach

Use [InterProcessCommunicationManagerTests](../../kernel/selftests/logic/InterProcessCommunicationManagerTests.cpp),
[SocketManagerTests](../../kernel/selftests/logic/SocketManagerTests.cpp),
[MemoryFileFixture](../../kernel/selftests/MemoryFileFixture.hpp),
[SocketFileSystemFixture](../../kernel/selftests/SocketFileSystemFixture.hpp)
and [communication integration](../../kernel/selftests/poststartkernel/communicationintegration.cpp).
Inspect empty/full/wrapped/packet pipes, closed endpoints, seals, listening and
connected/socketpair states, shutdown, queued messages and descriptor passing.
Compare known fixture owners and byte/queue changes, including short/failing
operations. Use controlled corrupt counts/private pointers and cyclic chains.
Run x86_64 QEMU/GDB at meaningful live fixture stops and debug/release checks.

## Work log

### 2026-10-18 — Created

P2: combines anonymous-file discovery, buffer/queue and reference-lifetime work
for pipes, memory files and UNIX sockets. No implementation has started.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
