# Track the process tree, task ownership and shared open resources

- Type: feature
- Priority: P1
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Maintain and expose the process tree plus process/task lifecycle, ownership,
credentials and open-resource relationships through DebugManager. Diagnose a
child that never becomes waitable, a retiring task still owned by a CPU, or a
descriptor/file kept alive by another task. Existing process/task commands show
mostly IDs, links, stack/context and address-space pointers; these cases currently
require manual cross-layer structure inspection.

Follow [the workflow](../TICKET.MD), required grilling and repository skills.
Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Depends on [foundations](../closed/build_debug_framework_foundation.md).
Related: [assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment),
[Logic documentation](../closed/document_logic_layer.md),
[Resource documentation](../closed/document_resource_layer.md).
This open ticket requires its own analysis and execution instruction.

### Scope and internal milestones

1. Add manager-owned ProcessManager, TaskManager, ProcessController and
   CredentialManager debug stats headers, with agreed maintained
   create/clone/exec/exit/retire/failure and current-state values. Define successful
   versus attempted operations and identify where each counter is updated once.
   Debug data and updates disappear from release builds.
   This ticket owns the ProcessController record and lifecycle counters;
   [memory work](../closed/add_debug_virtual_memory_views.md) can extend its agreed memory
   fields without creating a second registration or duplicate record owner.
2. Explicitly track process identity, parentage and lifecycle as creation,
   reparenting, stopping/continuing, exit and retirement occur. Provide a bounded
   tree showing active and retained/exiting processes, groups/sessions, parent
   task and exit recipient/wait owner. Handle roots, missing/partial parent
   records and slot reuse without attaching a new occupant to an old parent.
   No retained history after reclamation is authorized; settle identity and
   retirement publication in analysis rather than inventing permanent records.
3. Expose `Processes`, `RunningProcesses`, `Tasks`, `RunningTasks` and
   `SwitchingTasks`, slot allocation, CPU ownership/dispatch claims, process links,
   task names/context/stack, dying/cleanup state, memory/lifetime references and
   stop/exit/wait/retirement flags. These are the shared identities for other
   views. Scheduler policy and eligibility interpretation belong to
   [scheduling/waits](../feature/add_debug_scheduling_wait_views.md), not a duplicate reader.
4. Show task-specific descriptor/filesystem contexts as well as process defaults:
   shared tables/references, open entries/CLOEXEC, file status/offset/refcount,
   operations identity, inode/path and weak close/readiness observers. Deduplicate
   shared objects and cap descriptor scans independently of their stored count.
   Show cwd/umask, real/effective/saved/filesystem UID/GID, supplementary groups and
   context sharing without creating a separate CredentialManager database.
5. Return source identities/addresses for bounded owner/reference correlation
   used by memory, IPC and event views. Distinguish owning/weak/observed links;
   observed owners do not account for in-flight references or prove a leak.
6. Implement generic-family adapters and document native-reader roots, counts,
   slot/identity rules and lifetimes. Update the command guide and old
   `arx-proc`/`arx-task` migration ledger. Do not remove old commands here.

### Proposed interface and illustrative output

```text
arx-debug process tree --limit 64
  pid=1 parent=<root> state=active tasks=2
    pid=9 parent=1 state=exited waitable=false retirementActive=true
arx-debug process 9
  lifetimeUsers=1 memoryUsers=0 waitOwner=1 exitNotified=false
arx-debug task 12
  state=SWITCHING ownerCpu=1 dispatchUsers=1 process=9
arx-debug files --tid 12 --limit 32
  table=<addr> source=task references=2
  fd=3 file=<addr> flags=CLOEXEC offset=16 kind=pipe
```

Use the grammar approved in foundation analysis; example values are not runtime
evidence. Match actual kind through verified operation/type metadata. Bound strings,
tables and parent chains; never call target getters or acquire process/file locks.
Do not infer process failure solely from a stop inside a state transition.
Request-layer support, historical tracing and dump mechanics are excluded.

### Sources, integration and unknowns

- [ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp),
  [ProcessManager.cpp](../../kernel/layers/Resource/ProcessManager.cpp): process
  slots, credentials, descriptor/filesystem/signal shared records and references.
- [TaskManager.hpp](../../kernel/layers/Resource/TaskManager.hpp),
  [TaskManager.cpp](../../kernel/layers/Resource/TaskManager.cpp): task-specific
  contexts, running/switching slots, dispatch/retirement and cleanup state.
- [ProcessLifecycle.cpp](../../kernel/layers/Logic/ProcessLifecycle.cpp),
  [CredentialManager.cpp](../../kernel/layers/Logic/CredentialManager.cpp): lifecycle
  and security update sites; register production records through construction wiring.
- [VirtualFileSystem.hpp](../../kernel/layers/Logic/VirtualFileSystem.hpp):
  `file_t`, `inode_t`, `file_close_observer_t`; namespace readers belong to
  [filesystem work](../feature/add_debug_filesystem_views.md).
- Unknowns: retained-slot identity, parent publication during clone/exit,
  reparenting semantics, counter resets/overflow, table scan limits and how to
  describe borrowed/temporary references. Avoid changing process ownership rules.

## Acceptance criteria

- [x] Maintained process-tree and stats records correctly reflect creation, parentage changes, lifecycle and retirement without permanent historical retention.
- [x] Parent chains and process/task/CPU identities handle missing records and slot reuse; active and retained/exiting processes remain distinguishable.
- [x] Task ownership, context and retirement detail preserves useful old process/task output and supplies shared identities to dependent views.
- [x] Descriptor inspection honors task overrides, deduplicates shared contexts/files and distinguishes owning/weak references with bounded work.
- [x] Credentials/groups and filesystem context can be inspected without a synthetic manager database or inferior calls.
- [x] Partial/cyclic/unreadable state is reported explicitly; native-reader data rules and guide/migration updates are complete; release adds no debug work/storage.

## Verification approach

Use [ProcessManagerTests](../../kernel/selftests/resource/ProcessManagerTests.cpp),
[TaskManagerTests](../../kernel/selftests/resource/TaskManagerTests.cpp),
[ProcessControllerTests](../../kernel/selftests/logic/ProcessControllerTests.cpp),
[CredentialManagerTests](../../kernel/selftests/logic/CredentialManagerTests.cpp)
and [process lifecycle integration](../../kernel/selftests/poststartkernel/processlifecycletests.cpp).
Inspect controlled clone/share/exec/exit/wait sequences, task table overrides,
group changes and CPU switch/retirement windows in x86_64 QEMU/GDB. Include slot
reuse, missing parent, deep/cyclic debug records, a large sparse descriptor table
and partially published context. Compare maintained values with known completed
operations rather than another copy of the display implementation. Run the
appropriate debug/release build checks for changed update sites.

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

P1: the owner explicitly selected the process tree; lifecycle and shared-resource
identities underpin many other rich views. No implementation has started.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

### 2026-10-20 — Analysis started

Read the ticket, ticket workflow, required `mattpocock-skills:grilling` and
repository code skill. Read-only fact finding was delegated as required by the
grilling skill. Owner's existing preference for human-readable output applies.
Status remains open pending design confirmation; no implementation has started.

Source findings for design:

- Process IDs are reused slot indices (64 slots); tasks have 128 slots and a
  surviving task's numeric TID can change during exec. Neither has a generation.
- Process creation initially sets provisional parentage. Clone adjusts parent
  task/CLONE_PARENT before task publication; exec updates child parent-task IDs.
  Actual exit notification, retirement and reparenting also occur in Scheduler.cpp,
  so source-owned tracking hooks there are needed without changing scheduler policy.
- Raw slot allocation includes temporary exec-image processes. Separate slot
  allocations/reclamation from committed clone/exec/first-exit transitions.
- Task-specific descriptor/filesystem pointers override process defaults. Each
  owning context pointer is a reference; sharing a table does not multiply its
  direct per-file references for every task. Descriptor count is capacity, up to
  1,048,576, not occupied entries; independent scan/display ceilings are necessary.
- Only verified VFS descriptor routes allow decoding opaque handles as file_t.
  File kind needs verified operation metadata; inode type alone is insufficient.
  Readiness observers are weak links. Full namespace resolution belongs elsewhere.
- Credentials are process-inline; supplementary groups are immutable shared lists
  bounded by 65,536. Filesystem-ID setters can silently refuse a change while
  returning the old ID, so counters must follow actual mutation/outcome paths.

First design round will settle debug incarnation identity, maintained operation
semantics and readable command/resource-inspection choices. The ticket index
already lists this ticket once as P1/feature/open and remains accurate. No builds
or runtime checks were performed for this analysis.

### 2026-10-20 — Analysis and plan; owner design confirmation

Owner answered “yes to all” three design recommendations, confirming the shared
understanding below. Analysis is complete; implementation awaits the separate
execution instruction required by `tickets/TICKET.MD`. Preserve the existing
ticket workflow and scope, and prioritize human-readable output.

#### Agreed identity, state and counter semantics

- Add debug-only process/task slot-incarnation generations, independent of PID,
  mutable TID and slot address. Increment on slot reservation/reuse, capture
  referenced process/task generations when parentage or ownership is published,
  and clear occupant records at reclamation. Generation exhaustion makes the
  identity unavailable rather than wrapping and matching an older occupant.
  Retain only per-slot generation bookkeeping, current records and lifetime
  counters; no reclaimed process/task history or permanent ownership database.
- ProcessManager owns process records/tree and slot counters; TaskManager owns
  task records/slot counters. ProcessController owns lifecycle API counters;
  CredentialManager owns credential mutation counters without duplicating the
  credential database. Publish production roots through Dispatcher composition
  after the owning managers exist; temporary self-test managers do not register.
- Distinguish initializing/reserved records from published active processes and
  exited/retiring/retained records. Capture create-time parentage, clone overrides,
  exec parent-task changes and Scheduler reparenting. Missing, stale-generation,
  partially published and cyclic parents remain explicit instead of being
  attached to a new occupant of the same PID/slot.
- Keep raw slot allocation/reclamation distinct from successful published clones
  and committed execs: exec image preparation allocates temporary process slots.
  Count clone success at successful task publication, exec success at committed
  image/frame/sequence publication, first process exit at its actual transition,
  and task retirement/process reclamation at final slot reset. Count attempted
  operations, rejected/failed operations and deferred cleanup separately; a busy
  retirement attempt is not completed reclamation. Avoid duplicate counting in
  wrapper/internal helper paths.
- Current allocated/published/retained quantities follow real state transitions;
  detailed ownership/wait/stop/cleanup fields come from existing runtime records.
  Controller exit requests and actual first exits are separate observations.
  Wait outcomes distinguish consumed results, no-result/WNOHANG, blocked or
  interrupted/error returns. Stop/continue and reparent counts follow changes,
  not repeated notifications with no state change.
- Credential counters describe attempted operations, actual changes, no-ops and
  refusals/failures. Filesystem-ID operations return the old ID even when denied,
  so record their actual decision, not a zero-return heuristic. Supplementary
  group replacement counts on committed publication; exclude incidental internal
  credential getter calls from mutation totals.
- Stats have manager lifetime. Document unsigned-counter overflow semantics and
  no exact historical reconstruction after overflow. Mark relevant grouped
  updates/publication windows as partial; zero active writers is not a coherent
  cross-provider snapshot guarantee. All added types, storage, helpers,
  registration and update evaluation disappear with `DEBUG=0`.

#### Agreed readable views and bounded resource inspection

- Commands: `arx-debug process tree`, `process details --pid N`,
  `process files --pid N`, `process credentials --pid N`, `task list`,
  `task details --tid N`, `task files --tid N`, and generic `process stats`,
  `task stats`, `lifecycle stats`, `credentials stats`. Preserve the foundation's
  stopped-target/build-identity validation, numeric-only selectors and shared
  entry/byte/depth budgets. Reject unsupported or ambiguous selector combinations.
- Tree roots and children use deterministic slot order. Show PID, generation,
  parent identity/status, lifecycle, group/session, parent task, wait owner and
  exit recipient, including active and retained processes. Bound depth and
  parent chains; expose partial subtrees without inventing a complete history.
- Process/task details preserve useful legacy IDs, links, names, stack/context
  and address-space information. Add allocated slots, memory/lifetime users,
  dispatch claims, dying/cleanup state and stop/exit/wait/retirement flags.
  Show RunningProcesses, RunningTasks and SwitchingTasks CPU associations
  separately from stored ownerCpu; mismatch is an observed transitional state,
  not a scheduler-policy diagnosis. Other readers can use source addresses,
  slot/generation and current IDs as common correlation identities.
- Process file views inspect explicitly labeled process defaults. Task file
  views resolve the selected task's nonnull descriptor/filesystem overrides,
  falling back to that task's process defaults without executing Files()/Fs().
  Show cwd/umask, shared context addresses and reference counts. An equal default
  and task pointer still represents two owning context references.
- Descriptor count is capacity, not open entries. Add `--scan N` with default 256
  descriptor slots and maximum MAX_FILE_DESCRIPTORS (1,048,576), independently of
  displayed-entry and byte ceilings. Charge every scanned slot, including holes,
  against scan/byte work; report sparse/truncated scans explicitly. Enumerate
  each shared table and file once per command, preserving descriptor edges and
  CLOEXEC. Do not count every task sharing a table as a direct file reference.
- Decode file handles only for a verified VFS dispatcher route. Unknown/custom
  handles remain opaque. Show file offset/status/refcount, operation identity,
  inode/path identities and available inode metadata. Name a specific kind only
  when verified operation metadata supports it; otherwise show generic inode
  type and unknown implementation. Filesystem namespace/path reconstruction
  belongs to its existing ticket; show stored path components/identities here.
- Credentials are process-wide inline real/effective/saved/filesystem UID/GID;
  no task credential override or synthetic manager database. Read supplementary
  immutable shared groups with their stored count/refcount and bounded traversal
  (hard maximum 65,536). Bound cwd/task names and all other strings.
- Identify owning references, weak close/readiness observers and observed CPU
  associations distinctly. Bound observer lists and detect cycles. Callback,
  operation, private-data and context addresses remain opaque unless a verified
  layout explicitly authorizes interpretation. Observed owners neither explain
  every in-flight reference nor prove a leak.

#### Implementation steps, boundaries and affected code

1. Read the affected Resource, Logic, Dispatcher and self-test architecture
   sections plus kernel.cpp before code changes. Add manager-owned stats/root
   headers under debug/kernel/stats with explicit native-reader semantic IDs,
   roots/capacities, slot generations, count meanings and lifetime rules.
2. Add owner-side process/task tracking at allocation, initialization/publication,
   parent/identity updates, cleanup and final reset in ProcessManager.cpp and
   TaskManager.cpp, with matching header storage/API under DEBUG guards. Treat
   process array size 64, task array size 128 and BOOT_SMP_MAX_CPUS as real bounds.
3. Instrument ProcessLifecycle.cpp clone/exec/wait/exit operation boundaries and
   CredentialManager.cpp mutation decisions once. Add downward calls to the
   Resource-owned tracking interface from Scheduler.cpp at actual publication,
   stop/continue, first exit, notification, reparenting and retirement sites.
   This is lifecycle visibility only: no scheduling eligibility/policy reader,
   Request provider or change to runtime ownership/locking rules.
4. Register roots and any verified file-route/operation identities from existing
   construction/composition wiring. Keep ProcessController's single lifecycle
   record extensible by the virtual-memory ticket rather than adding a second
   owner or duplicate provider. Cross-manager roots are borrowed references.
5. Add adapters under debug/gdb/views and parser support for PID/TID/scan limits.
   Reuse generic bounded readers and keep result gathering separate from
   rendering. Summaries lead; selected details label units, identities, reference
   meaning, partial state and unavailable provenance in human-readable language.
6. Update GdbCommands.md, affected existing architecture guides and the migration
   coverage ledger for arx-proc/arx-task. Keep legacy commands intact. Document
   native-reader ordering, sentinels, slot/ID reuse, ephemeral shared objects,
   bounds and interrupted-update semantics beside matching layout headers.

#### Verification and remaining work

- Extend existing ProcessManagerTests, TaskManagerTests, ProcessControllerTests,
  CredentialManagerTests and process lifecycle integration fixtures. Compare
  known operation outcomes with counter deltas and actual committed/reset states;
  include failed creation/rollback, clone sharing/copying, exec TID changes,
  first exit versus repeated retirement, reparent/wait, credential no-op/refusal
  and group replacement. Use bounded safe fixtures for malformed data.
- Host adapter/parser fixtures cover slot reuse/stale generations, missing and
  cyclic parents, deep trees, interrupted publication, task-specific overrides,
  shared tables/files, sparse large descriptor tables, scan/display/byte limits,
  opaque handles, weak observer cycles and unavailable strings/types.
- Inspect x86_64 QEMU/GDB before/after publication and in populated lifecycle
  fixtures. Observe clone/exec/exit/wait, ownership and switching/retirement
  windows with one and multiple CPUs. Compare stored source identities and known
  transitions rather than another copy of the display algorithm. Preserve
  legacy coverage through typed fields, not its unsafe fallback discovery.
- Build separate debug/release outputs and run relevant existing framework and
  allocator regressions. Extend release preprocessor, symbol/layout/disassembly,
  argument-erasure and native-header checks for new update sites. Reuse/extend
  test tooling already under debug/tests instead of reconstructing it.
- Principal risks: mutable TID versus slot identity, provisional clone parentage,
  Scheduler-owned retirement/reparenting, temporary exec-image process slots,
  task override/default reference semantics and sparse descriptor capacity.
  No full namespace reader, historical trace, allocation provenance, universal
  reference/leak proof, backend callback execution or dump mechanics is included.

No design decisions remain open. No implementation, builds or runtime tests were
performed during analysis. The ticket remains in feature with status analyzed;
execution, its verification and subsequent owner closure authorization remain.

### 2026-10-20 — Execution authorized

Owner instructed “go” after analysis. Moved to in progress and began implementing
the agreed plan. Existing unrelated generated bytecode changes are preserved.

### 2026-10-20 — Owner-directed verification scope

The owner requested avoiding excessive verification on remaining tickets. Added
an explicit stopping rule and evidence-based limits on repeated or broader
checks. Acceptance criteria, implementation scope, status, and execution gates
are unchanged.

### 2026-10-20 — Ready for owner review

Completed debug-only process/task incarnation records, lifecycle and credential
outcome counters, production registration, and bounded human-readable process
tree, task detail, credentials and shared-file views. Parent and owner generation
checks prevent slot reuse from inventing relationships. Task context overrides,
shared tables/files, weak observers, partial updates and traversal limits are
explicit. Added matching native-reader contracts, command documentation,
architecture updates and legacy-command coverage evidence. Runtime ownership and
scheduling policy remain unchanged; no Request provider was added.

Verification completed:

- Debug and release x86_64 image builds passed.
- Process adapter/parser fixtures: 16 tests passed; existing memory adapter
  fixtures: 15 tests passed.
- Process release checks passed: preprocessing, symbols/disassembly, baseline
  layout comparisons and compilation against public native-reader roots.
- QEMU/GDB process checks passed with one and four CPUs, including actual
  clone/exec/exit/wait transitions and ownership fixtures. Final formatted output
  run passed: `/tmp/arx-process-debug-4-z9trcmkj/gdb.log`.
  Earlier passing one/four-CPU runs are retained under
  `/tmp/arx-process-debug-1-9kvnh6vv/` and
  `/tmp/arx-process-debug-4-9y8arkhk/`.
- Release target checks passed under `/tmp/arx-process-release-4-g0own8c2/`;
  existing framework/memory target regression passed under
  `/tmp/arx-memory-debug-4-u14_v1go/`.
- Reviewed affected layer boundaries, readable code/output, documentation and
  release exclusion. Whitespace checks passed.

An early target run exceeded its display budget by rendering raw saved floating
point state; replaced that dump with a compact context/stack summary and a typed
context address. The final target run passed with the readable output.

Incidental verification findings: an initial four-CPU run failed scheduler
syscall fixture cleanup, then passed on retry. Repeated unchanged-HEAD boots also
showed an intermittent SMP fixture cleanup failure. These are different fixtures;
the baseline comparison does not establish an identical defect or a root cause.
Final required process runs passed. No scheduler fix is included, and no further
baseline or unrelated-suite investigation is required for this ticket.

All acceptance criteria are met. No implementation TODOs or required checks
remain. Retained history, complete reference/leak accounting and filesystem
namespace reconstruction remain outside the agreed scope.

### 2026-10-20 — Completion

The owner authorized closure once the work was done, following the proposed move
to `tickets/closed/`. Confirmed completion and passing final target evidence;
closed this ticket and moved it to that destination. Process/task ownership,
shared-resource views, documentation and bounded verification tooling are complete.
