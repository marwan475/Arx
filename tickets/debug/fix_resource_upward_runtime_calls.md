# Remove upward runtime dependencies from Resource

- Type: debug
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:diagnosing-bugs

## Task prompt

The 2026-10-17 code-skill audit at revision `d057ba1` traced Resource execution
into Logic through Dispatcher bridges and installed callbacks. This conflicts
with [add-arx-code](../../docs/skills/add-arx-code/SKILL.md), which forbids upward
dependencies including callbacks and indirection. Upper layers may access any
lower layer directly.

Expected behavior: Resource supplies resource mechanisms without calling into
Logic policy or Logic-owned release operations. Preserve execution accounting,
scheduling-policy retirement, robust-futex/child-TID cleanup, and descriptor
retention/release. Removing hooks without preserving their work is not a fix.

Follow [TICKET.MD](../TICKET.MD). Read the Resource, Logic, Dispatcher, and relevant
platform guides. Creation does not authorize analysis or implementation; use
the required diagnosis skill when analysis is requested.

### Reproduce and inspect

1. In [TaskManager.cpp](../../kernel/layers/Resource/TaskManager.cpp), initially
   lines 101 and 538, follow `dispatcher_account_scheduling` into
   [Dispatcher.cpp](../../kernel/layers/Dispatcher.cpp), initially line 580,
   which invokes `Scheduler::AccountSchedulingCurrent`.
2. Follow `SetPriorityCleanup`, `PriorityCleanup`, and `retirementCleanup` in
   TaskManager. [SynchronizationManager.cpp](../../kernel/layers/Logic/SynchronizationManager.cpp)
   registers `CleanupPriority` and `CleanupRobust`; Resource invokes them during
   cleanup. Trace all retirement paths, including current and retired tasks.
3. Follow `CopyDescriptorTable`'s retain callback and `ReleaseDescriptorTable`'s
   `entry.release` in [ProcessManager.cpp](../../kernel/layers/Resource/ProcessManager.cpp).
   Production descriptors install `VirtualFileSystem::ReleaseFileHandle`, for
   example in [EventManager.cpp](../../kernel/layers/Logic/EventManager.cpp).
4. Search callback registrations as well as invocation sites. Record the actual
   call paths, ownership, execution context, and lifetime constraints before
   proposing relocation of responsibilities.

The audit also found Logic-owned file-backing callbacks installed in
[VirtualFileSystem.cpp](../../kernel/layers/Logic/VirtualFileSystem.cpp), initially
lines 612 and 628, and invoked by
[platform VMM](../../kernel/platform/memory/vmm.c). The skill names the
Request/Logic/Resource stack explicitly. Whether the same strict rule applies
below Resource is unresolved; record that boundary question during analysis
before expanding implementation into platform code.

### Documentation conflict and unknowns

[Resource.md](../../docs/architecture/layers/Resource.md), section 4, explicitly
allows these callbacks. [Logic.md](../../docs/architecture/layers/Logic.md) and
[Dispatcher.md](../../docs/architecture/layers/Dispatcher.md) also describe their
wiring. Those guides match current behavior but conflict with the new rule.
Identify the needed corrections and obtain explicit instruction before editing
documentation. Do not silently relax the skill to make the existing code pass.

No runtime failure was demonstrated by the audit. The replacement design,
feasibility of separating the three migrations, and affected runtime checks
remain unknown. Record a concrete plan and verification scope during analysis,
then wait for execution authorization. Coordinate signal header changes with
[the compile-time dependency ticket](fix_resource_signal_type_dependency.md).

## Acceptance criteria

- [ ] Diagnosis inventories Resource-to-Logic runtime paths, including callbacks and Dispatcher indirection, and resolves the platform scope question.
- [ ] An owner-authorized plan identifies where accounting, policy cleanup, and descriptor lifetime operations will run.
- [ ] The agreed Resource scope has no upward runtime dependencies; equivalent behavior is retained rather than discarded.
- [ ] Relevant existing checks for accounting/scheduling, task retirement and robust cleanup, and descriptor lifetime are selected and their results recorded.
- [ ] Documentation conflicts and required follow-ups are recorded; any explicitly authorized edits match the resulting architecture.
- [ ] Implementation follows `kernel.cpp` formatting and the agreed architecture boundary rules.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 for confirmed policy
violations with no demonstrated runtime regression. Current callback wiring is
documented and behaviorally significant; the migration design remains open.
Analysis and execution have not started.
