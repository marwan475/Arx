# Add pipe, memory-file and UNIX socket debug views

- Type: feature
- Priority: P2
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Expose IPC object state and ownership relationships so an investigator can
explain blocked reads/writes/connect/accept, missing pipe endpoints, socket queue
pressure and retained passed descriptors. Current commands can reveal some file
paths but do not decode these anonymous-file object graphs.

Follow [the workflow](../TICKET.MD), required grilling and repository skills.
Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Depends on [foundations](../closed/build_debug_framework_foundation.md) and
[task-aware descriptors](../closed/add_debug_process_resource_views.md). Reuse backing-page
identities from [memory views](../closed/add_debug_virtual_memory_views.md) and readiness
relationships from [signal/event views](../feature/add_debug_signal_event_views.md), without
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

- [x] Debug-only stats and collection roots expose IPC and UNIX socket state with clear transfer/count units and no release overhead.
- [x] Pipe/memory-file decoding verifies type/operations first and reports descriptor-reachable scope accurately.
- [x] Pipe rings/packets, memory seals/pages and socket peers/accept/message queues are bounded, correctly ordered and correlated with file owners.
- [x] Ancillary rights and credentials are inspectable without unsupported complete-reference/leak claims or automatic payload scans.
- [x] Invalid types, counts, links, missing endpoints and partial groups return explicit bounded status without target calls/locks.
- [x] Native-reader semantics, shared identities and command-guide examples cover every selected rich collection; Request/dump/history work is absent.

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

## Verification scope

Keep verification proportional to this ticket's changes. Select the smallest set
of checks that demonstrates the acceptance criteria and covers the affected
behavior. Run the relevant checks once after the final changes; stop when they
pass. Repeat or broaden checks only after a new change, a failure, or a concrete
unresolved risk, and record the reason. Do not add repeated QEMU boots, unrelated
suite runs, or baseline investigations merely for extra confidence. Record
incidental failures and their impact; investigate further only when evidence
makes them a blocker for this ticket. Preserve required acceptance checks and
report any checks that could not run.

## Work log

### 2026-10-18 — Created

P2: combines anonymous-file discovery, buffer/queue and reference-lifetime work
for pipes, memory files and UNIX sockets. No implementation has started.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

### 2026-10-20 — Owner-directed verification scope

The owner requested avoiding excessive verification on remaining tickets. Added
an explicit stopping rule and evidence-based limits on repeated or broader
checks. Acceptance criteria, implementation scope, status, and execution gates
are unchanged.

### 2026-10-20 — Analysis started and source findings

The owner selected pipes, memory files and UNIX sockets. Read the ticket and
workflow, applied required `mattpocock-skills:grilling`, and reviewed repository
code/documentation skills. Delegated read-only IPC/socket source fact finding as
required by grilling. Standing requirements remain readable code/output and
proportional verification. No implementation, builds or runtime checks started;
status remains open pending design confirmation.

Source findings for the design round:

- IPC has no object catalogue. ProcessDebugStats already publishes verified VFS
  descriptor-route and pipe/memory/socket operation-table identities. Reuse these,
  task descriptor-table overrides, shared-table/file deduplication and current
  process/task generations before decoding privateData. Queued ancillary rights
  provide additional owning file roots; this still is not a complete census.
- Pipes store a ring of at most 65,536 bytes with 4,096-byte atomic writes,
  weak reader/writer links, endpoint-object reference count, readiness state and
  two packet bits per physical byte (packet-byte/end). Packet order follows
  `(head + offset) % capacity`. A short packet read discards its unread tail:
  returned, dequeued and discarded bytes need distinct semantics. Default
  inspection must read fields/metadata without copying the embedded payload array.
- Memory files have 256 backing-page slots, embedded inode/seals and inode.backing.
  Backing/page objects and mappings may outlive the final memory-file close;
  mapping roots cannot reconstruct a freed memory_file_t. Correlate backing-page
  identities with existing memory readers and label the reachability limitation.
- SocketManager already owns a catalogue of at most 128 endpoints. Pending accept
  entries and queued ancillary descriptors hold owning file references; peer links
  are weak. Datagram peer asymmetry is legitimate. Admission tests pendingCount
  against `> backlog`, so backlog+1 is possible and must not be called corruption.
  UNIX addresses require bounded length-aware decoding, including abstract names.

Prepared owner choices: discovery without a new IPC catalogue; command grammar,
selectors and explicit bounded payload detail; transfer/result/queue counters.
Remaining source-level implementation details will follow the confirmed choices.
After agreement, record the full implementation and focused verification plan,
then follow the ticket's execution gate. The existing ticket index remains accurate.

### 2026-10-20 — Analysis and plan confirmed

The owner answered “yes to all” to discovery scope, command/payload choices and
transfer-counter semantics. Shared understanding is confirmed; the design frontier
is settled. This confirms analysis, not the separate execution instruction
required by TICKET.MD. Priority remains P2. No implementation has begun.

Agreed behavior:

- Discover pipes and memory files through verified process/task descriptors and
  queued socket rights. Reuse the existing socket endpoint catalogue. Add no IPC
  catalogue or detached-object census. Mapping-only backing remains inspectable
  through memory views; backing/page identity can outlive the original file.
- Commands: `ipc pipes`, `ipc memory-file`, `sockets endpoints`, and
  `sockets messages <numeric-endpoint>`. Support applicable `--pid`/`--tid` and
  `--fd` selectors; rights and credentials appear inline with messages. Numeric
  endpoint selection must match a verified current catalogue entry before decode.
- Default reads metadata only, including pipe packet-boundary metadata rather
  than copying the embedded 64-KiB payload. Explicit `--payload-bytes N` requests
  bounded remaining/logical payload detail, capped at 4,096 total bytes per
  command and charged to the shared read budget. Render bytes safely with escapes.
- Separate API outcomes and short returns from returned/copy/enqueue/dequeue/
  discard quantities. Zero-byte returns are successful. Internal retry loops do
  not create new call attempts. Provider callbacks are labeled provider calls,
  rather than claimed to be syscall totals.

Affected code and implementation steps:

1. Add IPC- and SocketManager-owned debug stats headers and production provider
   publication in Dispatcher composition. Publish typed descriptor/operation and
   endpoint roots, capacities and semantic versions. Keep temporary test-manager
   instances unregistered. Add debug-only object-to-stats links where static IPC
   callbacks need their owning manager; do not introduce global runtime lookups.
   Expose the existing object layouts through provider-owned named layout headers
   so native readers can use matching build headers without GDB-only type guesses.
2. Instrument existing creation/release, transfer, queue, control and readiness
   update sites in InterProcessCommunicationManager.cpp and SocketManager.cpp.
   Maintain creation/failure/current-object counts, transfer attempts/results,
   short successes, byte totals and queue/rights transitions with explicit scopes.
   Use atomic counters and writer markers around existing lock-protected changes;
   per-object markers cover pipe/memory state. Markers are diagnostics, not locks
   or seqlocks. Release builds erase tracking storage and argument evaluation.
3. Add shared bounded file-root/owner correlation helpers using existing process
   and task identities and descriptor-route validation. Respect task table overrides
   and process fallback, deduplicate shared tables/files, and preserve each observed
   owner edge. Verify operation identity before privateData interpretation. Retained
   ancillary files extend reachable roots with cycle/depth/edge/scan limits. Label
   owning, weak, observed-descriptor and potentially in-flight relationships; no
   complete reference accounting or leak/garbage-collection claims.
4. Implement metadata readers for pipe endpoint refs, ring capacity/head/size,
   logical packet boundaries and stored readiness; memory-file seals/inode size,
   page slots, backing objects and bounded matching mappings. Reuse existing
   backing/page identities. For sockets, validate the max-128 endpoint list/count,
   FIFO packet and accept chains, size/offset/count consistency, byte limits,
   shutdown/state/credentials and stored readiness. Address rendering distinguishes
   unnamed, pathname and abstract raw-byte names using lengths, preserving NULs.
   Ancillary chains have bounded counts (at most 253 files per record).
5. Register adapters using the common array/list readers. Extend numeric command
   selection and parser options only as needed for these views. Preserve independent
   entry/byte/depth/descriptor-scan budgets; report partial groups, invalid counts,
   null/mismatched private pointers, missing endpoints and cycles without retries
   waiting for stopped writers. No inferior calls or inspection-time locks.
6. Document exact roots, ownership/lifetimes, operation validation, logical ring
   ordering, selectors, units, transfer and sample interpretation in stats/layout
   headers and GdbCommands.md. Review affected Logic, Resource and KernelArch
   descriptions under add-arx-code/update-kernel-docs. Existing README debug-guide
   navigation suffices unless a meaningful new guide is required.

Source-specific safeguards and risks:

- Pipe endpoint-object references differ from file_t refCount. Packet reads can
  consume more bytes than copied; count the discarded tail independently.
- Socket PEEK copies bytes and may install descriptor rights without dequeue.
  TRUNC can return more bytes than copied and consume/discard a full message.
  A later stream error/wait can follow committed positive progress. Track actual
  commit/copy sites separately from the final return value.
- Datagram peer links can be legitimately unilateral. Connection-oriented
  asymmetry is reported as observation, with partial-state context. The current
  connect admission permits backlog+1 pending entries; validate chain/count
  consistency instead of imposing a different policy.
- internalReferences covers pending accepts and queued socket rights, not every
  live file reference. Parsed/detached controls, receive rollback and descriptor
  installation can be in flight outside persistent roots. Native/GDB views cannot
  reproduce the target's collector or prove exhaustive ownership.
- Avoid accidental payload reads caused by copying an entire pipe_state_t.
  Read metadata fields separately. Preserve existing synchronization and policy.
  The signal/event ticket is not required to finish first: show stored readiness
  and weak relationships without interpreting unavailable observer implementations.

Focused verification plan:

- Add a small set of deterministic host-reader fixtures covering wrapped/packet
  pipe ordering and closed endpoints; memory seals/pages/backing identity; UNIX
  abstract names; listener/peer/message/rights relationships; copied versus
  consumed/returned bytes; invalid types/counts/pointers/cycles, interrupted groups
  and budget truncation. Verify default readers do not copy payload bytes and
  explicit payload requests respect their cap.
- Build isolated x86_64 debug and release kernels and check release exclusion of
  provider storage/tracking symbols. Reuse the foundation's existing RAM-only,
  bounded-reading and identity checks rather than duplicating unrelated tests.
- Use one four-CPU QEMU/GDB boot with meaningful existing communication fixture
  stops to compare production roots/known state and maintained transfer/queue
  changes. Use IPC/SocketManager tests, MemoryFileFixture, SocketFileSystemFixture
  and communication integration as the source of scenarios. Let the boot's
  automatic tests run; do not add repeated boots or unrelated suite runs.
- After final changes, run the relevant checks once, then stop when they pass.
  Repeat or broaden only after a change/failure/concrete unresolved risk. Record
  unavailable checks and their impact. Finish with source/diff/format, native
  contract, documentation-link and ticket-index checks.

No unresolved scope questions remain. Status is analyzed in tickets/feature/;
await the owner's explicit execution instruction before moving to in_progress
and implementing this plan. Closure and destination authorization remain a later
owner-review gate after implementation and required verification.

### 2026-10-20 — Execution authorized

The owner instructed “go” after confirming the analysis. Moved the ticket to
in_progress and started the recorded implementation plan. Focused verification
and the metadata-only default remain mandatory.

### 2026-10-20 — Ready for owner review

Implemented manager-owned IpcDebugStats and SocketDebugStats, source-site atomic
operation/transfer/queue/lifetime counters, per-object IPC markers and existing-lock
socket transaction markers. Production providers publish through Dispatcher;
release erases tracking storage and argument evaluation. Shared IpcState.hpp and
SocketState.hpp layouts support exact-build native consumers.

Added ipc pipes/memory-file and sockets endpoints/messages adapters with verified
VFS routes, operation/inode/private-data identity, dependency-owned process/task
identities, bounded descriptor/ancillary discovery, owner edges, packet/ring FIFO
ordering, seals/backing/pages/mappings, UNIX address semantics and stored readiness.
Default reads exclude payload; explicit samples share a 4096-byte command cap.
API/short/zero/returned totals are separate from completed vector-copy fragments,
queue transitions, discarded packet tails and descriptor-right delivery/disposal.
Copies completed before later failure remain counted; unknown partial progress
inside an unsuccessful copy fragment is excluded. No catalogue was added for IPC.

Updated GdbCommands.md native/command/lifetime contracts and the affected Logic,
Resource and KernelArch descriptions. Existing README navigation remains valid.
No Request instrumentation, inspection-time locks/callbacks, history or dump work.

Verification was kept light as requested:

- Six focused stopped-memory host fixture tests passed through
  `python3 -B debug/tests/test_communication.py`: wrapped packet metadata and
  metadata-only reads, bounded payload samples, closed endpoints, invalid routes/
  rings/partial groups/truncation, abstract embedded-NUL addresses, queued rights
  and credentials, message offsets, invalid endpoints/cycles/counts, and memory
  seals/page/backing/mapping identities.
- Final isolated x86_64 debug image and release ELF builds passed. Release symbol
  inspection excludes CommunicationOperation, PublishDebug and IPC/socket debug
  stats. Build logs: /tmp/arx-ipc-debug-build.log and
  /tmp/arx-ipc-release-build.log. Build artifacts remain under /tmp/arx-ipc-*.
- One four-CPU q35 QEMU boot completed 160/160 automatic kernel cases, zero
  failures/skips/blocked, including IPC, SocketManager and communication/request
  integration. No repeated boots or extra kernel suites were run. Logs:
  /tmp/arx-ipc-boot.log and /tmp/arx-ipc-gdb.log.
- GDB observed a live pipe with eight queued bytes, shared descriptor owners,
  correct logical ordering and opt-in payload; then a live 11-byte memory file,
  its page/backing identities and seals. Both rich collections completed within
  bounds. A later final read of the same VM verified production stats, empty
  socket catalogue and explicit rejection of an absent numeric endpoint:
  /tmp/arx-ipc-finish.log. Socket send/receive attempts balance outcomes; recorded
  queued/unqueued rights were 494/494, installed rights 38, and all transfer calls
  had returned by the final sample. PEEK/TRUNC explain why installation/discard
  and copied/returned totals must remain independent.
- Verification limitation: the intended live queued-socket breakpoint was set
  after that earlier self-test had already run. Interrupting that wait exposed
  an empty catalogue, and its fixture-local `m` was unavailable; this was a GDB
  harness sequencing failure, not an adapter exception. Queued socket/rights
  decoding was covered by controlled host fixtures and target socket tests;
  live nonempty socket snapshot was not captured. Per the owner's instruction
  to finish promptly, no extra boot was added solely to repeat that fixture.
- Reviewed layer boundaries, shared runtime layouts, update scopes and readable
  formatting. Diff whitespace, release exclusion and ticket-index/link checks
  passed. Generated bytecode changes were removed from the worktree while
  preserving tracked cache contents. QEMU was stopped after inspection.

All acceptance behavior is implemented, with the live-socket snapshot limitation
above explicitly recorded. No implementation TODOs remain. Observed ownership is
bounded and incomplete by design, and no complete-reference/leak proof is claimed.
Status remains in progress awaiting owner review and explicit authorization to
close and move to tickets/closed/add_debug_ipc_socket_views.md.

### 2026-10-20 — Completion

The owner authorized closure with “close”, accepting the proposed destination
`tickets/closed/add_debug_ipc_socket_views.md`.
Implemented IPC and UNIX socket statistics, bounded collection/ownership views,
optional payload detail and native-reader documentation. Verification evidence
and the missed live queued-socket snapshot limitation remain recorded above.
