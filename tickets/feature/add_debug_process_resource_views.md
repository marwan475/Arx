# Track the process tree, task ownership and shared open resources

- Type: feature
- Priority: P1
- Status: open
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
Depends on [foundations](build_debug_framework_foundation.md).
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
   [memory work](add_debug_virtual_memory_views.md) can extend its agreed memory
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
   [scheduling/waits](add_debug_scheduling_wait_views.md), not a duplicate reader.
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
  [filesystem work](add_debug_filesystem_views.md).
- Unknowns: retained-slot identity, parent publication during clone/exit,
  reparenting semantics, counter resets/overflow, table scan limits and how to
  describe borrowed/temporary references. Avoid changing process ownership rules.

## Acceptance criteria

- [ ] Maintained process-tree and stats records correctly reflect creation, parentage changes, lifecycle and retirement without permanent historical retention.
- [ ] Parent chains and process/task/CPU identities handle missing records and slot reuse; active and retained/exiting processes remain distinguishable.
- [ ] Task ownership, context and retirement detail preserves useful old process/task output and supplies shared identities to dependent views.
- [ ] Descriptor inspection honors task overrides, deduplicates shared contexts/files and distinguishes owning/weak references with bounded work.
- [ ] Credentials/groups and filesystem context can be inspected without a synthetic manager database or inferior calls.
- [ ] Partial/cyclic/unreadable state is reported explicitly; native-reader data rules and guide/migration updates are complete; release adds no debug work/storage.

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

## Work log

### 2026-10-18 — Created

P1: the owner explicitly selected the process tree; lifecycle and shared-resource
identities underpin many other rich views. No implementation has started.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
