# Add signal, epoll and event-notification debug views

- Type: feature
- Priority: P2
- Status: in progress
- Required skill: mattpocock-skills:grilling

## Task prompt

Expose delivery state across SignalManager and EventManager to explain pending
signals, masked waits, epoll watches that no longer report, missing inotify events
and signalfd/eventfd behavior. Today these require hand-decoding process/task
bitmasks, disposition tables, weak file observers and several private queue types.

Follow [the workflow](../TICKET.MD), required grilling and repository skills.
Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Depends on [foundations](../closed/build_debug_framework_foundation.md) and shared task/
descriptor identities from [process/resource work](../closed/add_debug_process_resource_views.md).
Correlate waits with [scheduler/sync work](../closed/add_debug_scheduling_wait_views.md)
and producers with [IPC/socket work](../closed/add_debug_ipc_socket_views.md); missing optional
adapters must not prevent local state inspection. See
[assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment).
Execution authorization is recorded in the work log below.

### Scope and internal milestones

1. Add manager-owned signal/event stats headers and maintained delivery/queue/
   notification/failure counters and current values. Agree event meanings,
   coalescing/reservations and attempt-versus-success semantics; do not count
   every notification as a delivered user event. Compile all added work out of release.
2. Expose process/task pending masks, task masks/saved restoration, shared
   dispositions/actions, sender records, real-time queue records/count/reserved,
   alternate stack/delivery depth and process stop/exit state. Distinguish process-
   and task-directed signals and queued versus pending-bit summary semantics.
3. Show epoll instances and watch topology: descriptor/file identities, requested
   events/data, armed/pending/one-shot/edge/exclusive information where stored,
   sequence/topology generations, weak close observers and nested epolls.
   Display stored state and bounded source relationships; never call Poll or
   WatchReadiness to compute a new result inside the stopped kernel.
4. Show event counters/semaphore mode, inotify watches/masks and queued record
   ring order, overflow/ignored events, and signalfd masks plus matching pending
   signal relationships. Distinguish global manager notification lists from
   descriptor-discovered anonymous counter objects. Validate type/operation
   identity before privateData interpretation.
5. Implement bounded adapters and relationship lookups without a standalone
   graph engine. Specify roots/bounds, mask meanings, ring ordering, generation
   semantics and weak object lifetime so a future native consumer can recover
   the same information. Update `docs/debugging/GdbCommands.md`.

### Proposed interface and illustrative output

```text
arx-debug signals --pid 9 --tid 12
  processPending=[CHLD] taskPending=[RTMIN] masked=[RTMIN] depth=0
  queue.count=1 queue.reserved=0
arx-debug events epoll --tid 12 --fd 5 --limit 32
  watch fd=3 events=IN|ET|ONESHOT armed=false pending=false file=<addr>
arx-debug events notifications --limit 32
  kind=inotify watches=2 queued=3 overflow=false
arx-debug events counter --tid 12 --fd 6
  value=2 semaphore=true
```

Final grammar/mask labels are settled in analysis. Handle null/unpublished queues,
shared dispositions, expired weak files, corrupt count/index/links and incomplete
updates without target locks/calls. Cap nested topology and queue/record reads.
No signal consumption, notification injection, historical delivery tracing,
Request-layer providers or dump mechanics.

### Sources and unknowns

- [SignalManager.hpp](../../kernel/layers/Logic/SignalManager.hpp),
  [implementation](../../kernel/layers/Logic/SignalManager.cpp),
  [SignalState.hpp](../../kernel/layers/Resource/SignalState.hpp),
  [signal_types.h](../../klib/signal_types.h): actions, masks, sender/queue contracts.
- [ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp),
  [TaskManager.hpp](../../kernel/layers/Resource/TaskManager.hpp): ownership and
  process/task current signal state.
- [EventManager.hpp](../../kernel/layers/Logic/EventManager.hpp),
  [EventManager.cpp](../../kernel/layers/Logic/EventManager.cpp): epoll/watch/counter
  roots and generations.
- [EventNotifications.cpp](../../kernel/layers/Logic/EventNotifications.cpp):
  notification/watch/record/signal-descriptor types, fixed ring limits and overflow.
- [VirtualFileSystem.hpp](../../kernel/layers/Logic/VirtualFileSystem.hpp): weak
  `file_close_observer_t` relationships.
- Unknowns: private type exposure, stored versus interpreted readiness labels,
  group publication, queue reservation display, inotify overflow meaning and
  limits for nested epoll relationships. Preserve actual delivery behavior.

## Acceptance criteria

- [ ] Maintained debug stats have documented event/success/coalescing semantics and no release data/update cost.
- [ ] Process/task masks, dispositions, sender and queue/reservation state explain pending versus eligible delivery without consuming anything.
- [ ] Epoll watch/generation/weak-file state and eventfd/inotify/signalfd objects are readable with verified type identity and correct ordering/masks.
- [x] Nested topology, stale weak links, invalid ring counts and interrupted state terminate with explicit partial/truncated status.
- [x] Rich views never invoke target readiness/delivery methods or locks and do not claim historical evidence absent from records.
- [x] Native-reader semantics, related owner/wait references and guide updates are complete; no Request support or dump mechanisms are added.

## Verification approach

Use [SignalManagerTests](../../kernel/selftests/logic/SignalManagerTests.cpp),
[EventManagerTests](../../kernel/selftests/logic/EventManagerTests.cpp),
[signal integration](../../kernel/selftests/poststartkernel/signalintegration.cpp)
and [event integration](../../kernel/selftests/poststartkernel/eventintegration.cpp).
Inspect known masked/pending/queued signals, disposition sharing, alternate stack,
one-shot/edge-triggered watches, source close, event-counter transitions, inotify
overflow and signalfd filtering. Correlate known fixture descriptors/tasks rather
than relying solely on text snapshots. Test controlled nested/cyclic/stale objects,
oversized queues and unpublished records. Run x86_64 debug/release builds and
QEMU/GDB checks appropriate to changed managers; state untested modes explicitly.

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

P2: signal and event inspection share delivery, readiness, wakeup and observer
lifetime work. No implementation has started.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

### 2026-10-20 — Owner-directed verification scope

The owner requested avoiding excessive verification on remaining tickets. Added
an explicit stopping rule and evidence-based limits on repeated or broader
checks. Acceptance criteria, implementation scope, status, and execution gates
are unchanged.

### 2026-10-20 — Analysis started

Owner requested this ticket. Read TICKET.MD and the required grilling skill;
source inspection is underway before the design round. Existing command parsing
requires provider/view pairs and makes --pid/--tid mutually exclusive, so the
illustrative grammar needs an explicit decision. Implementation has not started;
shared-understanding confirmation and the subsequent execution instruction remain
pending under the repository workflow.

### 2026-10-20 — Analysis and plan

Applied mattpocock-skills:grilling and read the applicable add-arx-code and
update-kernel-docs repository skills. The owner answered "yes to all" to the
command grammar and separate accounting recommendations. Use `signals pending`,
`signals dispositions`, and `events epoll|notifications|counter`, with mutually
exclusive `--pid` / `--tid`; a selected task includes its owning process state.
Keep queue acceptance, coalescing, consumption, handler-frame delivery,
notification attempts, record copies, overflow and failures distinct.

Source findings:

- SignalState.hpp holds a compact RT record array, capacity 128, with
  `count + reserved <= 128`; RTMIN is 32. Standard pending signals preserve their
  first sender and ignored submissions may succeed without queueing. Pending
  masks summarize state rather than count individual queued records.
- Shared dispositions, process/task pending state, sender arrays, task saved mask,
  alternate stack/depth, and process stop/exit fields already exist. Static queue
  helpers have multiple producers; instrumentation must cover actual mutations.
- Epoll owns global instances and weak source-file observers. Stored armed and
  pending flags do not prove current readiness. Existing topology depth is five;
  sequence/topology/notification generations have different meanings.
- Global notifications are limited to 64 instances. Inotify has 128 physical watch
  slots and 64 FIFO records, read as `(head + i) % 64`. Exact newest-record
  duplicates coalesce; a full queue replaces the newest record with IN_OVERFLOW.
  Overflow is queued content, not a persistent flag.
- Signalfd matching uses the selected calling process/task pending union and its
  descriptor mask; a shared file has no intrinsic single task owner. Eventfd
  counters must be discovered through descriptors separately from notifications.
- Existing providers supply bounded process/task and descriptor identity lookups.
  Private anonymous event layouts currently live in implementation files.

Implementation steps:

1. Add manager-owned SignalDebugStats.hpp and EventDebugStats.hpp, publication in
   debug/kernel and Dispatcher initialization, and debug-only mutation counters
   with explicit attempt/success/coalescing/reservation semantics. Keep Resource
   independent of Logic and compile all debug storage/updates out of release.
2. Expose stable shared Logic event-state layouts and exact file operation
   identities for epoll, counters, inotify and signalfd. Publish manager roots,
   capacities, generations and process-provider references. Preserve behavior.
3. Add local bounded signal/event adapters and register semantic IDs. Pending
   output includes sender/RT queue/reservations, masks and restoration, alternate
   stack/depth and stop/exit state. Dispositions show shared identity/actions.
   Event views inspect stored watches/observers, counters, inotify rings and
   signalfd context matches only after validating type/operation identity.
4. Reuse --limit/--bytes/--depth/--scan budgets, enforce physical array limits and
   cap nested epolls at five. Detect cycles, invalid counts/indices, stale weak
   links, unpublished groups and interrupted updates; report partial/truncated
   status. Read large arrays by member. Never invoke target functions or locks.
   Optional wait/producer correlations must not gate local inspection.
5. Document native root/bound/lifetime/mask/generation contracts beside stats and
   update GdbCommands.md plus affected existing architecture descriptions using
   update-kernel-docs. No new guide, Request provider, dump or history mechanism.

Verification plan:

- Add focused stopped-memory reader fixtures for signal ownership/masks,
  compact queues/reservations, identity rejection, inotify wrapping/overflow,
  nested/cyclic/stale epolls, corrupt bounds and interrupted publication. Run
  focused fixtures and existing command parsing checks once after final changes.
- Build isolated x86_64 debug/release kernels, verify release exclusion, and use
  one focused QEMU/GDB session with existing SignalManager/EventManager and
  signal/event integration fixtures if a suitable toolchain is available.
- Current environment inspection found no make/compiler/QEMU/GDB on PATH and no
  installed WSL. Locate bundled Python for host fixtures. Unavailable required
  kernel checks must be recorded as incomplete verification; they prevent a
  completion claim. Do not substitute host fixture success for kernel execution.
- Review final diff for readable kernel.cpp formatting, layer boundaries,
  behavior preservation, native contracts and documentation/link accuracy.
  Broaden/repeat checks only for a concrete failure or unresolved risk.

Risks are static-helper accounting ownership, anonymous-type DWARF visibility,
weak-file lifetime and interrupted multi-field snapshots. Shared layouts,
operation identity checks, bounded member reads and explicit partial status
address them without introducing a target-side readiness calculation.

Analysis is recorded. No implementation files changed. Await the owner's
post-analysis execution instruction under TICKET.MD; on authorization move this
ticket to tickets/in_progress/ and update the index before implementation.

### 2026-10-20 — Execution authorized

Owner instructed "go" after the analysis handoff. Moved the ticket to
tickets/in_progress/ and started the recorded implementation plan.

### 2026-10-20 — Progress and remaining work

Implemented the recorded scope:

- Added SignalDebugStats.hpp and EventDebugStats.hpp, debug-only manager storage,
  counters at mutation/copy sites, provider publication and Dispatcher reservation.
  Signal counters use class-owned static storage because Queue/Consume/Discard
  and ReleaseExit are static and have several producers; totals explicitly include
  every instance and static helper caller, including selftests. Event totals are
  per instance. API accounting completes at the outer return after cleanup.
- Moved stored event layouts into Logic's EventState.hpp without changing their
  fields. Counter/epoll operation tables are named class-owned tables so readers
  can compare exact identities; existing Poll identities and behavior remain.
- Added arx.signals/1 and arx.events/1 adapters with the agreed command grammar,
  masks/actions/senders/compact queues/reservations, saved restoration/stack/depth,
  stop/exit state, epoll topology/observers/generations, descriptor-discovered
  counters, global notification lists, inotify FIFO/overflow/ignored records and
  contextual signalfd matches. Sequence addresses and descriptor owner edges
  support existing wait/producer correlation without optional adapters.
- Added shared entry/byte/depth and event scan ceilings, fixed array bounds,
  stale/cyclic/retiring-object checks, operation/route/inode/manager verification,
  and partial status for held locks, active writer groups and inconsistent state.
  Global event lists can be read without process-provider roots. Private inotify
  storage is read by member rather than copying the large object.
- Added 15 host reader/parser tests and two debug-only kernel accounting cases
  in SignalManagerTests.cpp and EventManagerTests.cpp. Updated GdbCommands.md,
  KernelArch.md, Dispatcher.md and Logic.md using update-kernel-docs. No new guide
  or diagram was needed; README navigation remains accurate.

Verification performed:

- Bundled Python command `python.exe -B -m unittest discover -s debug/tests
  -p 'test_*.py' -v`: all 29 tests passed (15 new signal/event/parser tests,
  14 existing communication/scheduling tests). Coverage includes process/task
  identity, sender/queue/reservation interpretation, blocked signalfd matches,
  ring wrapping/overflow/ignored records, wrong types, weak observer expiry,
  nested epoll cycles/depth, corrupt bounds, unpublished roots, active groups,
  locks and entry/byte/scan exhaustion.
- Initial focused 13 tests passed. Repeated host verification only after review
  changes: API accounting moved to final returns, scan budget became shared with
  descriptor discovery, wrong-FD diagnostics and wait/group correlations were
  added, and action flag labels were finalized. Final 29-test run passed.
- Source review confirmed no target calls/locks, no new upward layer dependency,
  no Request instrumentation, no history/dump support, and use of release-erasing
  debug macros for all new statistics/update expressions. Stored event fields and
  existing notification/delivery algorithms were preserved. Whitespace/diff and
  moved-ticket link/index checks were performed.

Required verification unavailable: no make, C/C++ kernel compiler, QEMU or GDB
was found on PATH or in the checked common LLVM/MSYS locations; WSL is not
installed. Therefore x86_64 debug/release builds, release-symbol exclusion,
the new kernel accounting cases, existing manager/integration suites and live
matching-ELF/DWARF GDB checks have NOT run. Host fixtures do not establish that
kernel compilation, linking or real-target private-type decoding succeeds.

Remaining work: use a supported kernel build environment for isolated x86_64
DEBUG=1 and DEBUG=0 builds, inspect release symbols, and run one focused QEMU/GDB
session with SignalManager/EventManager plus signal/event integration fixtures.
Exercise the five new commands on real fixture descriptors/tasks and verify
DWARF layouts/operation identities and queue/notification counter deltas. Fix any
compile/runtime failures, then confirm the remaining acceptance criteria. The
ticket stays in progress; it is not ready for closure or a completion claim.
