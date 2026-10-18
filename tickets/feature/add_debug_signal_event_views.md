# Add signal, epoll and event-notification debug views

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Expose delivery state across SignalManager and EventManager to explain pending
signals, masked waits, epoll watches that no longer report, missing inotify events
and signalfd/eventfd behavior. Today these require hand-decoding process/task
bitmasks, disposition tables, weak file observers and several private queue types.

Follow [the workflow](../TICKET.MD), required grilling and repository skills.
Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Depends on [foundations](../closed/build_debug_framework_foundation.md) and shared task/
descriptor identities from [process/resource work](add_debug_process_resource_views.md).
Correlate waits with [scheduler/sync work](add_debug_scheduling_wait_views.md)
and producers with [IPC/socket work](add_debug_ipc_socket_views.md); missing optional
adapters must not prevent local state inspection. See
[assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment). This is an open ticket,
not authorization to implement it.

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
- [ ] Nested topology, stale weak links, invalid ring counts and interrupted state terminate with explicit partial/truncated status.
- [ ] Rich views never invoke target readiness/delivery methods or locks and do not claim historical evidence absent from records.
- [ ] Native-reader semantics, related owner/wait references and guide updates are complete; no Request support or dump mechanisms are added.

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

## Work log

### 2026-10-18 — Created

P2: signal and event inspection share delivery, readiness, wakeup and observer
lifetime work. No implementation has started.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
