# Design the Arx debug framework and create implementation tickets

- Type: feature
- Priority: P2
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Design a replacement x86_64 debug-build framework and produce its implementation
backlog. The owner is an experienced kernel developer; focus on useful subsystem
information and reducing the context agents must gather to diagnose failures.
This replaces the earlier proposal to extend `scripts/gdb_extention.py` with more
independent commands. Preserve the historical discussion in the work log.

The agreed design uses root `debug/kernel/` and `debug/gdb/` directories,
manager-owned custom maintained stats structs under `debug/kernel/stats/`, and
a named DebugManager registry stored in Dispatcher. GDB discovers records through
that registry, uses matching-ELF type information for generic inspection, and can
provide richer Python view adapters through one command family. Support C
platform state as well as C++ managers. Exclude Request-layer manager support.

Tracking, records and the framework must compile out in release builds without
runtime cost. Reads must work at arbitrary stops without target calls or locks,
with explicit unavailable/partial state and bounded traversal. Track early PMM/heap
state before Dispatcher and register it afterward. Shared helpers support safe
concurrent counters and selected grouped-update detection. Do not require one
uniform manager stats layout or globally coherent snapshots.

Include every selected rich view in
[the source-backed assessment](plan_gdb_extension_tickets.md#rich-view-assessment),
including maintained process-tree/lifecycle information and the previously agreed
scheduler, VFS and initramfs views. Preserve useful coverage of all existing
commands before removing the old script and commands; update launch integration.
All rich-view data must be interpretable by a future native dump consumer, not
only a Python formatter. All dump mechanics are explicitly deferred.

Include a command guide under `docs/debugging/` and a repository agent skill for
adding stats/views during debugging or proposing follow-up improvements afterward.
Coordinate with [the agent-skill planning ticket](../feature/plan_kernel_agent_skill_tickets.md)
to avoid duplicating its broader QEMU/GDB usage skill. This ticket creates the
backlog; kernel/framework, view, documentation and skill implementation belong
to the resulting child tickets.

Follow [the ticket workflow](../TICKET.MD). During analysis use
`mattpocock-skills:grilling` to establish the owner's debugging pain points,
preferred command behavior, target architectures, and investigation priorities.
Record an analysis and plan, set this ticket to `analyzed`, and wait for the
owner's explicit execution instruction before carrying out the ticket-creation
work. Detailed inventory, selection, and child-ticket creation are execution
deliverables; initial inspection supports the analysis.

### Investigation and deliverables

1. Inventory existing commands, their arguments, data sources, pointer discovery,
   output, and current limitations. At creation time the script registers
   `arx-platform`, `arx-pmm`, `arx-vmm`, `arx-cpus`, `arx-pci`, `arx-heap`,
   `arx-vfs`, `arx-initramfs`, `arx-proc`, `arx-task`, and `arx-rsrc`.
   Recheck this inventory against the current script when executing.
2. Map existing coverage and the owner-selected views onto the new framework.
   Explore scheduling/run queues and blocked tasks, process lifecycle, address
   spaces/page faults/TLBs, locks and wait queues, signals, events, sockets/IPC,
   descriptors/VFS, DMA, interrupt/syscall state, and self-test progress/results.
   Use the assessment and final scope discussion to distinguish selected views
   from conditional/new-instrumentation ideas. Request-layer managers are excluded.
3. For each worthwhile opportunity, identify a concrete debugging question,
   existing manual investigation steps, source-backed data that a command could
   expose, and how it would improve diagnosis. Distinguish kernel tracked records,
   rich-view contracts/adapters, shared infrastructure and migration/cutover work.
4. Consider x86_64 feasibility across boot phases, symbol/type
   availability, pointer resolution, stopped versus running targets, partial or
   corrupt state, traversal bounds, and the cost of reading target memory.
   Favor read-only inspection; any proposed state-changing behavior requires an
   explicit rationale and agreement during the child ticket's analysis.
5. Create one reasonably bounded feature ticket per selected addition or
   improvement in `tickets/feature/`, following the ticket guide. Each needs:
   - A descriptive filename, agent-assigned priority, status `open`, and required
     skill `mattpocock-skills:grilling`.
   - The failure scenario or investigation question, intended command interface
     and example output, relevant source symbols/structures, proposed integration
     points, constraints, dependencies, unknowns, and scope boundaries.
   - Observable acceptance criteria and a verification approach using relevant
     kernel/self-test state or controlled fixtures, including unavailable or
     invalid target state where applicable.
   - A link back to this parent ticket and related documentation tickets where
     they provide useful context. Child tickets retain their own analysis and
     execution gate; this investigation does not authorize their implementation.
6. Record a concise inventory and gap assessment in this ticket's work log, with
   links to the created tickets and a suggested implementation order. Record
   rejected or deferred candidates with brief reasons to avoid repeated research.
   Check existing active and closed tickets to avoid duplicate proposals.

This ticket produces a debugging-framework backlog. Keep implementation changes
in the resulting tickets. Record confirmed defects in current tooling as debug
tickets requiring `mattpocock-skills:diagnosing-bugs`, and distinguish them from
feature opportunities. Agree the final number and grouping of tickets during
analysis rather than inventing work to meet a count.

## Source entry points

- [GDB extension](../../scripts/gdb_extention.py)
- [Debug launcher](../../scripts/debug-kernel.sh)
- [Build and QEMU targets](../../Makefile)
- [Architecture contracts and implementations](../../arch)
- [Platform services](../../kernel/platform)
- [Layer managers and Dispatcher](../../kernel/layers)
- [Self-test infrastructure and suites](../../kernel/selftests)
- [Project structure documentation ticket](../closed/document_project_structure.md)
- [Self-test documentation ticket](../closed/document_self_tests.md)

## Acceptance criteria

- [x] Existing commands and meaningful coverage gaps are inventoried against the current implementation.
- [x] Child scopes reflect the agreed DebugManager design, debug-only tracking, directory layout, Request exclusion and full selected rich-view coverage.
- [x] The migration ticket requires preserved information before removal; native-dump readability is specified while dump mechanics remain deferred.
- [x] Selected proposals address concrete debugging scenarios and identify inspectable kernel state and extension integration points.
- [x] Feasibility, architecture/boot-phase constraints, robustness, dependencies, and unknowns are recorded for each proposal.
- [x] Selected additions and improvements have individual, actionable feature tickets with priorities, detailed prompts, acceptance criteria, and verification approaches.
- [x] Child feature tickets start `open`, require grilling during analysis, and link back to this ticket; duplicate tickets are avoided.
- [x] The work log links all created tickets, recommends an implementation order, and explains deferred candidates.
- [x] Owner authorizes closure and destination; the completion entry and move then preserve this ticket's full history.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned investigation and backlog creation for Arx-specific GDB
debugging extensions. Command inventory was checked to ground this prompt;
analysis and execution have not started.

### 2026-10-18 — Analysis started

Read `tickets/TICKET.MD`, the required grilling skill, and the repository skills.
Initial inspection confirms the eleven command registrations listed above.
`arx-proc` currently exposes allocation, address-space pointers, and task links;
`arx-task` exposes allocation, stack, links, and the raw architecture context.
These are starting points for discussing richer diagnostic views, not a completed
inventory or selected backlog. The debug launcher supports x86_64 and aarch64.

Owner discussion is pending on debugging pain points, architecture priorities,
command behavior, and investigation breadth. Ticket grouping and count will be
agreed after those choices and source-backed feasibility checks. Status remains
`open` until the grilling conversation and analysis plan are complete. No child
tickets or implementation changes have been made.

### 2026-10-18 — Owner scope clarification

The owner selected x86_64 only, requested subsystem information views similar
to the existing commands, and delegated recommendations for other useful
debugging extensions. The owner also requested command documentation and raised
an agent skill for adding extensions during debugging or capturing an opportunity
afterward. The existing agent-skill planning ticket already covers a broader
QEMU/GDB usage skill; the proposed extension-authoring workflow should have a
distinct scope and shared references.

Initial source inspection identifies task/scheduler state, sleep/futex waiters,
process lifecycle, richer address-space views, and self-test progress as useful
discussion candidates. Shared pointer discovery and traversal robustness need
assessment as prerequisites. These are provisional groupings, not an approved
selection. Confirm documentation scope, skill behavior, and final ticket grouping
before completing analysis. No execution has started.

### 2026-10-18 — Expanded analysis proposal; owner confirmation pending

The owner agreed to the command-guide and extension-authoring-skill scope and
requested a wider assessment rather than limiting selection to the original six
extension areas. The working proposal is now **19 feature tickets**: 17 extension
or shared-tooling improvements, one command guide, and one authoring skill.
Names below describe scopes rather than finalized command interfaces. This count
and grouping await the owner's confirmation; no child tickets have been created.

| Proposed ticket scope | Concrete question and source-backed starting point |
| --- | --- |
| Shared discovery and safe inspection | Can commands find initialized managers and stop safely on damaged state? Extend `ArxPlatform`/`ArxResourceManagers` to Logic/Request data, bounded traversal, and clear unavailable-state reporting; avoid inferior calls. |
| Task and scheduler views | Which tasks are runnable, running, switching, or misplaced? `task_t`, `TaskScheduling.hpp`, TaskManager running/switching arrays, and Scheduler accounting/queues. |
| Sleeps, futexes, and priority inheritance | What is a task waiting on and what recorded owner could unblock it? Scheduler sleep entries and SynchronizationManager waiter/PI tables. |
| Process lifecycle and identity | Why is a process stopped, exiting, retained, or not waitable? `process_t` parent/exit/wait/retirement fields, references, credentials and groups; enrich `arx-proc`. |
| Descriptors and open files | What does this task/process descriptor reference, and which tables/files are shared? `task_t` overrides, `descriptor_table_t`, filesystem context, and `file_t`; correlate with existing VFS views. |
| Address spaces and backing | What mappings and permissions does an inactive process own? `virt_addr_space_t`, `virt_region_t`, backing objects/shared pages; enrich `arx-vmm`. |
| x86_64 page-table translation | Which entry or permission causes a mapping discrepancy? `arch_paging.c`, address-space `pt`, HHDM metadata, and x86 paging constants; inspect a selected VA/root without calling target helpers. |
| Signals | Which signals are pending, masked, ignored, queued, or being handled? Process/task signal fields, shared dispositions, and `SignalState.hpp`. |
| Events and readiness | Why does an epoll/eventfd/inotify/signalfd consumer not wake? EventManager generations, epoll/watch lists, counters and notification state; inspect stored readiness rather than invoke poll callbacks. |
| UNIX sockets | Which endpoint, peer, backlog, receive queue or passed descriptor is blocking progress? SocketManager endpoint list, endpoint/packet/ancillary structures in `SocketManager.cpp`; no network-transport claims. |
| Pipes and memory-file IPC | Are pipe readers/writers or capacity preventing progress, and what memory-file backing/seals exist? Descriptor-discovered private state in `InterProcessCommunicationManager.cpp`; do not assume a global IPC registry. |
| DMA allocations | Which CPU, physical and device addresses belong to a retained/releasing allocation? DmaManager allocation records, mapping cookies, constraints, backend pointers and counts. |
| CPU interrupt/IPI state | Is preemption deferred or a TLB/reschedule mailbox pending? Enrich CPU inspection with `cpu_info_t`, syscall-active state, IPI mailbox/lock, IDT and recorded interrupt routing; RAM state only by default. |
| Syscall and exception frames | What syscall arguments, return context or fault bits are present at this stop? Decode explicit `arch_syscall_frame_t`/`registers_t` pointers and available stopped registers; validate provenance and stack bounds. |
| Clocks and process timers | Which clock was selected, and which deadlines/CPU timers are armed? `arch_time.c` sources/active_source, TimeManager availability, process timer/accounting fields and stored LAPIC calibration. Show last sampled time honestly. |
| Self-test progress/results | Which case is active or failed? SelfTestManager instance, active-case timing, phase/context, result and detail tables. |
| Boot error log | Which recorded diagnostics were published before the failure? `klib.c` static `boot_errors`, bounded `error_log_buffer_t`, ready flags and reserved count; distinguish unpublished and capacity-exceeded entries. |
| Command documentation | Plan `docs/debugging/GdbCommands.md`, README navigation, launch/load prerequisites, existing command reference, arguments, example output, interpretation and limitations. Future extension tickets must update this reference. |
| Extension-authoring agent skill | Plan a repository skill under `docs/skills/` for extensions needed during debugging and opportunities recognized afterward: reuse existing views, ground reads in sources, implement bounded read-only inspection, verify and document. Propose follow-up tickets for work outside current authorization. Link the separate QEMU/GDB usage-skill backlog. |

Proposed execution plan after analysis approval:

1. Move this ticket to `in_progress` and perform the detailed current-command
   inventory, duplicate check, and source/feasibility assessment required above.
2. Develop each agreed scope into a bounded ticket with concrete interface/output
   examples, source integration points, dependencies, unknowns and acceptance
   criteria. Separate defects from features after confirming their behavior.
3. Prioritize discovery/robustness and foundational task/process/descriptor/memory
   views before dependent subsystem correlations. The command guide can document
   existing commands independently; skill authoring depends on verified tooling
   and guide examples. Assign individual priorities based on findings.
4. Verify ticket metadata, links, uniqueness, scope boundaries and index entries.
   Each implementation ticket must specify meaningful x86_64 GDB/fixture or QEMU
   checks for normal, unavailable, invalid and corrupt/partial state as relevant.
   This planning ticket itself does not require a kernel build or boot unless
   needed to confirm a tooling defect or feasibility claim.
5. Record the full inventory, final ticket links, dependency order and deferred
   candidates. Leave the parent in progress for owner review and closure approval.

Constraints and exclusions: x86_64 only; read-only stopped-target inspection,
concise default subsystem summaries with optional detail and bounded reads. A
stop may occur mid-update, so report inconsistencies rather than assert corruption
or a proven deadlock. Raw spinlocks contain no owner metadata. Defer general
lock-owner/deadlock proofs, historical syscall/interrupt tracing, automatic
arbitrary-task unwinding, live MMIO/MSR probing, and kernel instrumentation.
Private `.cpp` types/static symbols and HHDM reachability need verification with
the selected ELF and stop phase. Never fabricate runtime parity or readiness by
calling inferior methods.

Potential existing defects requiring confirmation during execution include the
missing successful return in `_extract_first_hex` and stale raw TaskManager
layout/capacity fallback. They are not counted as feature opportunities and are
not yet filed as confirmed debug tickets. If the agreed 19 scopes materially
change during detailed execution, record a revised plan and obtain owner direction
as required by the ticket workflow.

Status remains `open` while final scope confirmation is pending. Documentation
and skill scope are settled; the expanded ticket grouping is the remaining owner
decision before recording the completed analysis and setting `analyzed`.

### 2026-10-18 — Framework redesign discussion

Before approving the expanded proposal, the owner requested a redesign around
a debugging framework: an API that classes use to expose important diagnostic
state, reducing the need to author a new GDB command for each manager. The owner
explicitly requested grilling on this design. The 19-ticket proposal above is
superseded as the proposed plan; its subsystem findings remain research inputs.
No backlog count, framework architecture, implementation or execution is approved.

Reopen the design decisions in prerequisite order: kernel-side versus host-side
participation, stopped-target versus cooperative execution, current state versus
historical instrumentation, and intended consumers. Then settle schema and data
types, collection/object discovery and lifetime, consistency/overhead, registration,
API ergonomics, specialized diagnostic escape hatches, migration, documentation,
agent-skill changes and implementation-ticket grouping. Preserve x86_64 as the
current target. Existing command-guide and authoring-skill intent remains valid,
but their details must follow the framework design. Status stays `open` during
the interview.

### 2026-10-18 — Information-gathering focus

The owner confirmed arbitrary-stop inspection is required and narrowed the
current design discussion to information gathering. PMM is the concrete example:
free/allocated blocks and NUMA-node-specific state selected by the subsystem as
important debugging information. Other prior framework questions remain unsettled;
do not treat them as accepted through this answer.

Source check: `zone_t` in `pmm.h` stores total/free/used pages, per-order free-list
heads, physical regions, buddy metadata and HHDM information. `numa_node_t`
contains both ordinary and DMA zones, CPU count and proximity domain.
PhysicalMemoryManager is a C++ facade over C platform allocator state, so
subsystem exposure cannot assume all diagnostic data is stored in class members.
The existing `arx-pmm` computes free-block totals by traversing free lists; it
does not expose a stored allocated-block count. A new contract must distinguish
direct fields, derived aggregates and any newly maintained counters, and must
not equate used pages with allocated blocks.

Next interview frontier for information gathering: where exposure definitions
live, which data shapes are supported, direct/derived versus duplicated state,
and summary/detail collection cost. Historical instrumentation and additional
consumers are deferred discussion topics, not agreed implementation scope.

### 2026-10-18 — Maintained debug statistics and standalone dumps

The owner clarified that managers should maintain statistics as kernel work
happens, rather than relying primarily on debugger-side derivation. Tracking and
the framework operate only in debug builds, with no tracking/runtime framework
cost in release mode. This supersedes the earlier recommendation to prefer
debugger-derived statistics; exact build/link removal requirements remain to be
specified and verified in the implementation plan.

The same tracked information must be useful for kernel dumps on real hardware
without an attached debugger. Therefore the framework needs a kernel-side dump
consumer as well as arbitrary-stop GDB reads; GDB must not be required to collect
or interpret the dump. Dump format, destination, trigger, crash-path guarantees,
record data types, publication/freshness semantics and bounded storage remain
open decisions. Do not assume a memory-dump file, filesystem writer, serial-only
destination, historical event recorder, or coherent cross-CPU snapshot has been
authorized. The current design scope remains information gathering and exposure.

### 2026-10-18 — Manager-owned records; dump mechanics deferred

The owner accepted counters/current values/bounded tables and last safely
published information with incomplete-state reporting. Each manager provides its
own struct of important tracked information; do not force all managers into a
strict common statistics layout or mandatory field vocabulary. A small shared
discovery/inspection contract is still to be agreed.

The owner explicitly deferred **all dump mechanics**. Standalone real-hardware
dumps remain a future use case, but this analysis must not include implementation
of dump destinations, output formats, triggers, panic hooks, filesystem/serial
writers or crash-storage mechanics. Earlier recommendations about readable serial
reports and manual/panic triggering are future discussion inputs only. Current
work concerns debug-only maintained records and debugger information gathering.

Next decisions: whether ordinary GDB views discover manager-specific struct
layouts through matching debug symbols; how records are registered and tied to
instance lifetimes; and how generic inspection treats nested structs, arrays and
pointers without requiring a new command per manager. No execution is authorized.

### 2026-10-18 — Dispatcher-owned DebugManager and richer views

The owner accepted matching-ELF type-based GDB inspection, nested struct/array
expansion and explicit pointer-target inspection. The owner then specified one
kernel instance per manager and a `DebugManager` stored in Dispatcher holding
pointers to the managers' stats; GDB discovers records through that manager.
This replaces the earlier multi-instance registry recommendation for current
scope. Exact DebugManager ownership/storage and record registration mechanics
remain to be designed, with release-mode exclusion preserved.

Stats alone are insufficient: the owner explicitly requested richer information
such as VFS contents, InitRamFileSystemManager contents, and scheduler queues.
These must remain available without executing target manager methods. Clarify
whether rich views reference original kernel collections or maintained debug
projections and how traversal/formatting is supplied through one command family.

Initial source checks: InitRamFileSystemManager owns a fixed Archives array and
ArchiveCount; Scheduler has fixed sleep entries linked by array indices and
per-CPU heads, and scheduling state also resides in task records; VirtualFileSystem
has a mount list and dentry cache rather than a single universal child-array
shape. Native struct introspection alone cannot infer these collection semantics.
VFS 'contents' also needs a distinction between namespace/entry metadata and file
payload bytes. Dispatcher creation precedes layer manager creation, so entries
must express unregistered/uninitialized state; pre-Dispatcher discovery remains
unavailable unless separately designed. Dump mechanics remain deferred.

### 2026-10-18 — Replacement framework under root debug directory

The owner explicitly directed replacement of the existing debug commands and a
GDB-side framework redesign. Create a root `debug/` directory with separate
kernel DebugManager and GDB extension areas. Each manager supplies a debug stats
header describing its custom tracked data, and the GDB extension discovers that
data through DebugManager in Dispatcher. This is agreed replacement intent,
not authorization to remove commands before analysis is complete.

The earlier proposal to retain/enrich individual `arx-*` commands is superseded.
Agree subdirectory names, placement of per-manager headers, registration/type
discovery, rich-view integration and migration coverage before recording the
final implementation plan. Avoid assuming the previous round's recommendations
about live collection roots, Python adapters and payload display were accepted;
the owner has not answered those choices explicitly. Prior requirements remain:
x86_64, debug-build-only tracking/framework with no release runtime cost,
arbitrary-stop inspection, manager-owned custom structs, one kernel instance per
manager, generic struct/array inspection, command docs and agent authoring skill.
Standalone hardware dump usefulness remains a future requirement; all dump
mechanics are deferred.

### 2026-10-18 — Framework layout and migration choices confirmed

The owner answered yes to all four preceding recommendations:

- Use `debug/kernel/DebugManager.hpp`, `DebugManager.cpp`, per-subsystem headers
  under `debug/kernel/stats/`, and `debug/gdb/arx.py`, `framework/`, `views/`.
- DebugManager exposes a named registry of record addresses and type identities,
  independent of individual manager stats layouts. Construction wiring registers
  production managers; temporary fixtures do not replace production registrations.
- Stats records may expose typed references to original collections. GDB may use
  optional per-subsystem Python adapters with shared bounded traversal helpers,
  without executing target manager methods or copying full runtime collections.
- Preserve the useful information from all existing commands through the new
  framework before removing the old script/commands, then add selected new views.

The available codebase-design skill was read to guide a small shared interface
with registration, discovery and safe traversal kept in the framework. Source
inspection confirms DEBUG=1 supplies debug symbols while default DEBUG=0 does
not, and C/C++ runtime tracking must be conditionally excluded. Platform PMM/heap
initialization precedes Dispatcher creation, so early tracking and later registry
publication need distinct lifetimes. Remaining owner decisions concern early-boot
visibility, SMP publication expectations, complete versus cached VFS contents,
and the final revised implementation-ticket grouping. Status remains `open`.

### 2026-10-18 — Early tracking, publication, VFS scope and future rich dumps

The owner confirmed early debug tracking in platform PMM/heap with later
registration in Dispatcher-owned DebugManager; before Dispatcher publication,
registry-based GDB discovery reports unavailable. Shared debug update helpers
should support concurrent counters and detecting interrupted selected grouped
updates without imposing a uniform manager stats layout. No global coherent
snapshot is promised, and inspection never acquires manager locks.

The owner also confirmed a separate richer VFS view for complete visible contents
of supported mounted filesystems, beyond the initial migration of accurately
labeled mount/cache information. That future view must account for backing data,
runtime entries and mount relationships using bounded read-only traversal.

The owner asked whether rich views such as scheduler queues can also be dumped
when standalone dump support arrives. Treat dump-readiness of rich-view data as
a framework design requirement: expose discoverable typed collection roots,
counts/bounds and required relationships, with source-backed traversal/selection
semantics. Do not make GDB Python the only place where the information needed to
interpret a collection is documented or discoverable. In particular distinguish
Scheduler's stored indexed sleep lists from derived runnable/eligible task views.

This does not implement dumping or promise that Python adapters run in the
kernel. A future native dump consumer needs bounded traversal/selection and
rendering of these records/roots; arbitrary custom structs and pointers alone do
not supply generic kernel reflection. Shared declarative descriptions versus
native per-view readers remain implementation design choices to resolve when
specifying the rich-view contract, without reopening all deferred dump mechanics.
Do not imply that scalar-stat output alone preserves queue entries or VFS content.

### 2026-10-18 — Additional manager rich-view assessment

At the owner's request, inspected existing Resource, Logic and Request manager
headers and relevant implementations for additional rich views. Recorded the
source-backed candidates, data roots, constraints, grouping suggestions and
new-tracking requirements in
[debug_framework_rich_views.md](plan_gdb_extension_tickets.md#rich-view-assessment).

Strong candidates beyond the already requested VFS/initramfs/scheduler views:
process/task ownership and retirement, task-aware descriptors/shared open files,
futex waiters/PI owners, signal delivery records, epoll/readiness and notification
queues, UNIX socket peer/accept/message/ancillary relationships, pipes/memory
files, mappings/backing pages/pending writeback, PFN/buddy fragmentation, heap slab
occupancy/address lookup, DMA allocation records and process timers/clock state.
Credential information fits a process view. Self-test results, boot diagnostics
and CPU/IPI progress are useful additional registered records outside the ordinary
manager set. Observed resource-sharing correlation can build on these roots.

Request managers generally route to the underlying state and do not justify
duplicated rich collection families. ResourceFileSystem belongs with the VFS and
initramfs contents work. A persistent ELF image catalogue is not currently
available during arbitrary production stops: exec metadata is temporary and
production `elfMetadata` is null; retaining debug image metadata would be new
instrumentation. Automatic frame discovery/history and general spinlock-owner
inference likewise lack sufficient existing state.

Verification: source inspection only, with analysis links and whitespace checked;
no runtime verification, command implementations, dump mechanics or child tickets.
The assessment informs the remaining selection/grouping discussion. The previous
19-ticket proposal stays superseded; no replacement count is approved yet.

### 2026-10-18 — Exclude Request registration; select process tree

The owner explicitly excluded Request-layer managers from DebugManager support.
Do not create their registry entries, debug stats headers, update instrumentation
or rich-view adapters. This supersedes earlier candidate references to Request
manager discovery/counters. Existing shared command migration and CPU/platform
inspection should use appropriate Resource/Logic/platform roots instead.

The owner explicitly selected tracking the process tree. Include maintained
debug-visible process identity/parentage and lifecycle state, with a bounded tree
view for active and retained/exiting processes. Plan creation/clone, reparenting,
exit and retirement/reuse handling; identify parents by valid process identity,
not a potentially reused slot address alone. Missing/partial parents and
interrupted updates must remain inspectable. Historical retention after process
reclamation, exact record representation and lifecycle publication details have
not been agreed and must not be silently added. Updated the rich-view assessment
to reflect the Request exclusion and process-tree selection. No implementation
or child-ticket creation has started.

### 2026-10-18 — All rich views confirmed; final grouping proposal

The owner confirmed that all the previously listed rich views are included, not
only the process tree. Updated the current task prompt and ticket index to
describe the replacement framework. Explicitly conditional/deferred ideas such
as retained ELF history, automatic frame history and general lock-owner tracking
remain outside this confirmation. Request-layer managers and dump mechanics
remain excluded.

The revised grouping proposal is **25 feature tickets**: two framework foundation
tickets, twenty bounded view/provider families, one migration/cutover ticket and
two supporting documentation/skill tickets. This is packaging of the agreed
scope, not 25 separate GDB commands. Final grouping confirmation is pending.

| Group | Proposed child scope | Initial priority |
| --- | --- | --- |
| Foundation | Kernel DebugManager, record/collection contracts, registration, publication helpers and release exclusion | P1 |
| Foundation | Generic GDB discovery, struct rendering, named-view adapters and bounded read/traversal helpers | P1 |
| View/provider | PMM/NUMA statistics, ordinary/DMA zones, fragmentation and selected PFN detail | P1 |
| View/provider | Heap/slab statistics, occupancy, metadata pools and object-address lookup | P2 |
| View/provider | Process tree/lifecycle, retirement, credentials and shared process context | P1 |
| View/provider | Task/CPU ownership, scheduler policy/eligibility, sleeps and accounting | P1 |
| View/provider | Task-aware descriptors, shared tables, open files and observer relationships | P1 |
| View/provider | Futex waiters, PI owners/donations and wake/requeue state | P1 |
| View/provider | Address spaces, regions, backing/shared pages and pending writeback | P1 |
| View/provider | Selected x86_64 page-table translation and effective permission explanation | P2 |
| View/provider | VFS mount/cache migration and complete supported mounted-namespace contents | P1 |
| View/provider | Initramfs archive statistics, entries and path hierarchy | P2 |
| View/provider | Signal masks, dispositions, queues and current delivery state | P2 |
| View/provider | Epoll watches, event counters and notification queues/masks | P2 |
| View/provider | UNIX socket peers, accept/message queues, ancillary rights and readiness | P2 |
| View/provider | Pipes and memory-file state discovered through typed file relationships | P2 |
| View/provider | DMA backend/allocation records, constraints and releasing/rollback state | P2 |
| View/provider | Clock configuration/samples, process timers and deadline correlation | P2 |
| View/provider | Self-test active phase/case, results, failure details and completion state | P2 |
| View/provider | Published boot error records, pending entries and capacity accounting | P2 |
| View/provider | Platform/CPU/PCI migration, IPI/preemption progress and boot readiness | P1 |
| View/provider | Bounded observed resource-sharing correlation across existing view roots | P3 |
| Migration | Coverage ledger, launcher integration, parity verification and removal of old commands/script | P1 |
| Supporting | Human command guide and maintainer reference under docs/debugging, README navigation | P2 |
| Supporting | Agent skill for adding stats/views and capturing follow-up opportunities | P2 |

Proposed execution of this planning ticket, after analysis confirmation and the
owner's separate execution instruction:

1. Move the parent to `tickets/in_progress/`, preserving all history and fixing
   links. Recheck active/closed tickets, old command arguments/output/data sources,
   selected source structures and scope boundaries against the current tree.
2. Write each agreed child ticket with concrete failure scenarios, owned debug
   fields/update sites, registration and typed rich-view roots, interface/output
   examples, dependencies, unknowns and bounded acceptance/verification criteria.
   State what is directly stored, maintained debug data or interpreted collection
   state. No Request instrumentation and no dump mechanics.
3. Specify foundation checks for DEBUG=1/DEBUG=0 builds, absence of record storage,
   registration/update work and side-effectful update argument evaluation in
   release, arbitrary-stop reads, missing symbols, early boot, partial publication
   and corrupt/truncated collections. Use appropriate self-test/controlled fixtures
   and x86_64 QEMU/GDB verification in the implementation tickets. Do not claim
   this analysis has performed those implementation checks.
4. Record dependencies: kernel/GDB foundations first, PMM/initramfs as early
   interface exercises, then migrated existing coverage and foundational process/
   task/descriptor/memory views. Cutover removes old commands only after the
   coverage ledger passes; it need not wait for unrelated later enhancements.
   Rich view/provider tickets include their own maintained records and adapters,
   source semantics and future-native-reader contract, plus guide updates.
5. Give the authoring skill its distinct triggers and follow-up-ticket behavior,
   require skill-creator/writing-for-agents guidance during authoring, and link it
   to the separate generic QEMU/GDB usage-skill work. Documentation can begin with
   the agreed contract and is completed against verified behavior.
6. Validate all child metadata, priorities, source/back links, unique filenames
   and TICKET_LOG entries; record final inventory, links, ordering and deferrals.
   Finish ready for owner review; parent closure remains a separate authorization.

Risks/child-analysis details: type names and DWARF availability; record/root
lifetimes; mutable weak pointers and transient slot reuse; update-group publication
with multiple CPUs; definition of manager counters/units; collection limits;
backend-aware complete VFS namespace enumeration; retaining no hidden release
work; and native-reader semantics for manager-specific rich views. These do not
authorize new history buffers, per-manager inferior callbacks or blanket copying
of runtime object graphs. Confirm defects before filing separate debug tickets;
do not repair a script already scheduled for removal merely to expand this scope.

Analysis verification so far: source inspection, forty assessment links checked,
and whitespace checks passed. No kernel build/boot or implementation test ran;
this planning task has not created child tickets. Status remains `open` until
the final grouping is confirmed, then set `analyzed` and await execution instruction.

### 2026-10-18 — Consolidated grouping proposal

The owner requested fewer tickets by combining common workloads. Replace the
25-ticket packaging above with the following **12-ticket proposal**, preserving
all agreed framework, maintained stats, rich views, migration, documentation and
skill scope. No child tickets exist yet, so this changes the analysis plan rather
than moving or merging implementation tickets. Final grouping confirmation is
pending; status remains `open`.

| # | Proposed child scope | Shared workload and boundaries | Priority |
| --- | --- | --- | --- |
| 1 | Kernel/GDB framework foundations | Implement DebugManager, registry and record/rich-view contracts, publication helpers, release exclusion, generic GDB discovery/rendering and safe collection helpers together. Verify the small producer/consumer interface end to end with controlled records; no real subsystem rollout hidden in this ticket. Start the common guide/reference. | P1 |
| 2 | Physical allocation diagnostics | PMM/NUMA ordinary/DMA zones and fragmentation/PFN detail, heap/slab occupancy and address lookup, DMA allocation/rollback records. Shared physical allocation/lifetime scenarios and per-node address relationships; preserve device-address distinctions. | P1 |
| 3 | Process/task ownership and open resources | Maintained process tree, lifecycle/retirement, task running/switching ownership, credentials/groups, descriptor tables, shared open files and filesystem contexts. These share process/task slots, clone/exec/exit publication and reference-lifetime fixtures. Scheduling policy selection stays in #4. | P1 |
| 4 | Scheduling, waits, synchronization and timers | Runnable/policy/accounting views, sleep queues, futex/PI ownership and requeue, clock availability/samples and process real/CPU timers. Shared blocked-task/deadline/wakeup diagnosis; depends on #3 identities and ownership. | P1 |
| 5 | Virtual memory and translation diagnostics | Process address spaces, regions/protections, backing/shared/dirty pages, pending writeback and bounded x86_64 page-table explanation. Shared address/offset/permission and retention scenarios. | P1 |
| 6 | Filesystem contents and namespace diagnostics | Initramfs stats/entries/hierarchy, ResourceFileSystem interpretation, VFS mounts/cache/overlays and complete supported visible namespace. Shared path/mount/backend enumeration, with cached versus complete views kept distinct. | P1 |
| 7 | IPC and UNIX socket diagnostics | Pipes, memory files, endpoints, peers, accept/message queues, ancillary rights, readiness and descriptor ownership correlations. Shared anonymous-file discovery, privateData type verification and file/reference lifetime fixtures; uses #3 descriptor roots. | P2 |
| 8 | Signals and event delivery diagnostics | Process/task signal masks/dispositions/queues, epoll watches, counters, inotify records/watches, signalfd masks and weak close observers. Shared delivery/readiness/wakeup and notification lifetime scenarios; uses #3/#4/#7 where relevant without reimplementing their readers. | P2 |
| 9 | Platform, CPU and PCI diagnostics | Existing platform/CPU/PCI coverage plus boot availability, per-CPU preemption/syscall-active/IPI state, address-space and task correlation. Uses platform/architecture roots; no Request manager registration, instrumentation or callbacks. | P1 |
| 10 | Self-test and recorded failure diagnostics | Self-test phase/case/results/failure details and bounded published boot error records. Shared fixed diagnostic tables, publication states, truncation and boot-failure verification. | P2 |
| 11 | Cutover, compatibility verification and command guide | Maintain old-to-new coverage ledger, update launcher/build/source integration, verify all existing useful coverage, remove old script/commands after parity, complete docs/debugging/GdbCommands.md and README navigation. Depends only on required migration coverage, not every unrelated new enhancement. | P1 |
| 12 | Debug framework authoring agent skill | Repository skill for selecting/adding tracked fields and rich views during debugging, verifying/debug-only exposure and updating docs, or proposing a follow-up ticket afterward. Reuse the guide/common contract and link the distinct QEMU/GDB usage-skill work. | P2 |

Consolidation mapping from the 25-ticket proposal: foundations merge into #1;
PMM/heap/DMA into #2; process/task ownership/descriptors into #3; scheduler/futex/
clock-timer work into #4; VMM/page walk into #5; VFS/initramfs into #6; sockets/IPC
into #7; signals/events into #8; platform/CPU/PCI into #9; self-tests/error log into
#10; migration and the documentation ticket into #11; skill remains #12.

The separate observed resource-sharing graph ticket is absorbed into #3/#5/#7/#8:
each view resolves its own owners/weak links through shared identity and traversal
helpers. No independent universal graph engine or global leak/deadlock proof is
required. TaskManager slot/retirement work belongs to #3; Scheduler policy/wait
interpretation belongs to #4, so shared fields are not implemented twice.

Every implementation ticket includes updates to the common guide for its actual
behavior; #11 owns final command-reference completeness and migration checks.
This avoids postponing all documentation until cutover. Each combined ticket
must retain explicit internal milestones, per-view acceptance criteria and
subsystem-specific verification. Framework discovery/robustness is tested once
through the shared interface; view checks exercise meaningful subsystem state.
No selected rich view was dropped to obtain this smaller count. All previous
scope exclusions and the parent's analysis/execution gate remain in effect.

### 2026-10-18 — Analysis and plan confirmed; execution authorized

The owner accepted the consolidated 12-ticket grouping and instructed: "ok good
make the tickets". This confirms shared understanding and explicitly authorizes
the ticket-creation phase. The analysis gate is satisfied; status transitions
through `analyzed` to `in progress` in this entry and the parent moves to
`tickets/in_progress/`. No additional execution confirmation is required for
creating the agreed backlog. Child implementation remains unauthorized.

Execute the consolidated plan above: recheck the old command inventory, create
twelve detailed open feature tickets, link sources/dependencies/verification and
native-reader requirements, update the current index and record all findings.
Keep Request-layer support and dump mechanics excluded. Finish this parent ready
for review without closing or moving it until the owner authorizes closure and
its destination.

### 2026-10-18 — Ready for owner review

Created the twelve agreed consolidated feature tickets. All start `open` and
require `mattpocock-skills:grilling` during their own analysis. Each records its
investigation questions, milestones, proposed shared-family interface/output,
source/update sites, dependencies, exclusions, unknowns, observable acceptance
criteria and meaningful normal/invalid/partial-state verification. No child
implementation is authorized.

| # | Created ticket | Priority |
| --- | --- | --- |
| 1 | [Kernel and GDB framework foundations](../feature/build_debug_framework_foundation.md) | P1 |
| 2 | [Physical allocation, NUMA, heap and DMA](../feature/add_debug_physical_allocation_views.md) | P1 |
| 3 | [Process tree, task ownership and shared resources](../feature/add_debug_process_resource_views.md) | P1 |
| 4 | [Scheduling, blocked tasks, futexes and timers](../feature/add_debug_scheduling_wait_views.md) | P1 |
| 5 | [Virtual memory, backing and x86_64 translation](../feature/add_debug_virtual_memory_views.md) | P1 |
| 6 | [Initramfs and supported VFS namespace contents](../feature/add_debug_filesystem_views.md) | P1 |
| 7 | [Pipes, memory files and UNIX sockets](../feature/add_debug_ipc_socket_views.md) | P2 |
| 8 | [Signals, epoll and event notifications](../feature/add_debug_signal_event_views.md) | P2 |
| 9 | [Platform, CPU, IPI and PCI](../feature/add_debug_platform_cpu_pci_views.md) | P1 |
| 10 | [Self-test progress and boot-error records](../feature/add_debug_selftest_error_views.md) | P2 |
| 11 | [Cutover and replacement command documentation](../feature/migrate_debug_commands_and_document_framework.md) | P1 |
| 12 | [Agent stats/view-authoring skill](../feature/create_debug_framework_authoring_skill.md) | P2 |

Also created one P2 open
[legacy discovery defect ticket](../debug/resolve_legacy_gdb_discovery_defects.md),
requiring `mattpocock-skills:diagnosing-bugs`. The parent requires confirmed defects
to be distinguished from feature opportunities. Grouped missing hex return,
language restoration and obsolete manager/array layout assumptions into this
single debug workload. It allows justified interim repair or verified removal/
safe replacement; it does not add a framework feature or commit the owner to
repairing a tool already scheduled for deletion.

Inventory and findings:

- [Legacy inventory/coverage baseline](../feature/migrate_debug_commands_and_document_framework.md#legacy-command-inventory)
  covers all eleven registrations, arguments, roots, discovery, outputs, limits,
  fallbacks and replacement owners. Old getters can execute inferior functions;
  stale raw offsets cannot serve as a parity oracle.
- [Rich-view assessment](plan_gdb_extension_tickets.md#rich-view-assessment) records source
  data, feasibility and deferrals. All selected rich-view families are assigned,
  including future-native-reader requirements while dump mechanics stay deferred.
- ProcessController/CredentialManager stats belong to #3; ProcessController memory
  fields and ElfMapper operation stats belong to #5 without duplicate registration
  or persistent image history. Platform C roots are explicitly covered.
- Checked active/closed tickets for duplicates. Documentation tickets are context;
  the existing agent-skill planning ticket is coordinated with #12 so it does not
  create a duplicate authoring skill. Generic QEMU/GDB usage remains distinct.

Suggested implementation order:

1. #1 establishes the kernel/GDB contract with controlled records. #2 and #6 give
   early real PMM/array/path exercises; #3 and #9 provide shared process/task/
   platform identities and old coverage. Providers may use partial adapter milestones.
2. #4/#5 build on those roots. #7/#8 add file/readiness/delivery correlation without
   mandatory circular dependencies. #10 can proceed once registry/publication works.
3. Start #11's ledger/guide alongside providers; remove legacy commands only after
   required old coverage passes, without blocking on unrelated enhancements.
   Resolve the debug ticket through justified repair or verified decommission.
4. #12 uses verified stats/rich-view examples and the implemented guide. All
   providers update documentation incrementally. Resource correlation is integrated
   into owning views; no independent universal graph engine is required.

Deferred/rejected: Request-layer support (owner exclusion); AArch64 framework
expansion; dump sinks/formats/triggers/panic hooks/native dump execution;
persistent ELF/syscall/IPI histories; automatic arbitrary-task frame discovery/
unwinding; spinlock-owner inference without instrumentation; complete detached
IPC/backing catalogues or universal leak/deadlock proofs. Explicit typed-frame
inspection can be proposed later with a concrete caller/lifetime; it is not an
automatic frame-capture requirement in this workload.

Verification performed: source command/manager/fixture inspection; isolated
AST/stub reproductions of valid-hex `None` and incorrect language restoration;
metadata, filename uniqueness, complete index, local-link and whitespace checks.
All twelve features and the debug ticket have correct initial skills/status and
working parent/dependency/source links. No kernel/GDB target ran; runtime checks
belong to implementation. No implementation code, launcher, old command, real
command guide or skill was changed/created by this planning execution.

Planning deliverables are complete. Remaining action: owner authorization to
close this parent and confirmation of destination. Propose
`tickets/closed/plan_gdb_extension_tickets.md`; keep status `in progress` here
until that instruction under `tickets/TICKET.MD`.

### 2026-10-18 — Completion and recommended implementation sequence

The owner authorized closure in response to the proposed destination:
"what order should i have the tickets be done and you can close the ticket".
Closed this planning ticket and moved it to the proposed
`tickets/closed/plan_gdb_extension_tickets.md`, preserving its work log. The
deliverables are twelve open consolidated feature tickets, one grouped open
legacy-discovery debug ticket, source assessments/inventory and the current index.
This closure does not execute or close any child ticket.

Recommended sequential implementation order:

| Order | Ticket | Reason |
| --- | --- | --- |
| 1 | [Framework foundations](../feature/build_debug_framework_foundation.md) | Establish the kernel/GDB interface, registration, publication and release exclusion before providers. |
| 2 | [Platform, CPU, IPI and PCI](../feature/add_debug_platform_cpu_pci_views.md) | Validate real bootstrap discovery and CPU identities used by later correlations. |
| 3 | [Physical allocation, NUMA, heap and DMA](../feature/add_debug_physical_allocation_views.md) | Exercise C platform records, maintained stats and bounded lists with useful allocation evidence. |
| 4 | [Self-test and boot-error records](../feature/add_debug_selftest_error_views.md) | Small bounded diagnostic records improve evidence collection for the remaining work. |
| 5 | [Process tree, task ownership and resources](../feature/add_debug_process_resource_views.md) | Establish shared process/task/file identities and lifecycle ownership. |
| 6 | [Virtual memory and translation](../feature/add_debug_virtual_memory_views.md) | Build mapping/backing ownership on process identities and physical-address interpretation. |
| 7 | [VFS and initramfs contents](../feature/add_debug_filesystem_views.md) | Establish paths/mount/backend interpretation before communication object views. |
| 8 | [Scheduling, waits, futexes and timers](../feature/add_debug_scheduling_wait_views.md) | Correlate task identity, CPU ownership and address/backing-based waits. |
| 9 | [IPC and UNIX sockets](../feature/add_debug_ipc_socket_views.md) | Use descriptor/file, backing and namespace foundations for queue/peer ownership. |
| 10 | [Signals and events](../feature/add_debug_signal_event_views.md) | Complete delivery/readiness correlation with the task, wait and producer views. |
| 11 | [Cutover and command guide](../feature/migrate_debug_commands_and_document_framework.md) | Finish parity/integration checks and remove old commands once useful coverage is verified. |
| 12 | [Authoring agent skill](../feature/create_debug_framework_authoring_skill.md) | Teach the verified interface, real examples and maintained guide. |

This is a useful serial sequence, not a requirement to wait for unrelated work:
platform, physical allocation and self-test/error providers can proceed
independently after foundations; process views unlock memory and scheduling, and
filesystem work can overlap where its local roots suffice. Begin the cutover
ledger and guide during foundations/provider work; step 11 is final integration
and removal. New enhancements beyond old parity do not have to delay removal.
Resolve the legacy-discovery debug ticket alongside foundations/cutover through
justified repair or verified decommission, without copying its fallbacks.

Updated parent/dependency links, current ticket index and both required filename
closure histories. Verify links, index metadata/order and whitespace after the
move. No implementation/runtime verification is implied by this planning closure.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

## Rich-view assessment

Archived reference from the removed `tickets/analysis/` directory. Historical
commands and paths below describe the original investigation.

### Debug framework: rich-view assessment

Source inspection for [the GDB framework investigation](../closed/plan_gdb_extension_tickets.md),
2026-10-18. The owner subsequently confirmed inclusion of the listed rich views;
they are not implemented commands or execution-authorized child tickets. Target:
x86_64 debug builds. Dump mechanics remain deferred. Ideas explicitly marked as
requiring new tracking or lacking existing state remain conditional/deferred.

The agreed framework uses manager-owned stats headers under `debug/kernel/stats/`,
a named DebugManager registry in Dispatcher, and generic GDB inspection with
optional Python view adapters. Rich-view roots, bounds and interpretation must
also be specified for a future native dump consumer; Python presentation alone
does not make a view dumpable. All reads are bounded and do not execute target
methods or acquire target locks. A stopped target can be inside an update.

Owner clarification after this assessment: Request-layer managers are excluded
from DebugManager support. The process tree is explicitly selected for maintained
debug information, rather than remaining only an optional candidate.

#### Findings by owning manager

##### ProcessManager / ProcessController: process and retirement relationships

- Question: why has a child not become waitable, or why are process resources retained?
- Show parent/child relationships, process groups/sessions, parent task, wait owner,
  exit recipient, stop/continue/exit flags, retirement state, memory/lifetime users,
  associated tasks, address space and shared contexts.
- Owner-selected process tree: maintain debug-visible identity, parentage and
  lifecycle information as processes are created, reparented, exit and retire.
  The view should show retained/exited processes as well as active ones while
  their records remain valid. Define slot reuse and missing/unavailable parent
  handling explicitly; do not attach a reused slot to the wrong parent. A retained
  historical tree after process reclamation has not been requested.
- Sources: [ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp)
  (`Processes`, `RunningProcesses`, `process_t`),
  [ProcessLifecycle.cpp](../../kernel/layers/Logic/ProcessLifecycle.cpp).
- Fixed process slots are a natural root. Reconstruct relationships from IDs and
  pointers; diagnose missing/reused targets without assuming a stored child tree.
  Current fields explain state, not the historical sequence of lifecycle events.

##### TaskManager / Scheduler: CPU ownership and policy eligibility

- Beyond the agreed sleep view, show task-to-process ownership, running/switching
  tasks per CPU, affinity/placement, dispatch claims, retirement resources,
  effective versus base priority, and FIFO/RR/deadline policy state.
- Sources: [TaskManager.hpp](../../kernel/layers/Resource/TaskManager.hpp)
  (`Tasks`, `RunningTasks`, `SwitchingTasks`, `task_t`),
  [TaskScheduling.hpp](../../kernel/layers/Resource/TaskScheduling.hpp),
  [Scheduler.hpp](../../kernel/layers/Logic/Scheduler.hpp),
  [Scheduler.cpp](../../kernel/layers/Logic/Scheduler.cpp).
- There is no conventional ready-list root: scheduling scans task slots and
  evaluates state, affinity, placement, process state and policy. Label stored
  state separately from an interpreted eligibility view. Sleep queues use fixed
  slots, per-CPU index heads and a sentinel; index validation differs from pointer
  list validation. Do not claim to predict execution ordering from a slot dump alone.

##### ProcessManager / VFS: descriptor and open-file sharing

- Question: what does fd N really refer to, and which tasks/processes share its lifetime?
- Show task-specific versus process-default descriptor tables, entry flags,
  table references, file offsets/status flags/refcounts, operations identity,
  inode/path, filesystem context/cwd/umask, and close/readiness observers.
- Sources: `descriptor_table_t`, `filesystem_context_t` in
  [ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp), task
  overrides in TaskManager.hpp, and `file_t`, `inode_t`, `file_close_observer_t`
  in [VirtualFileSystem.hpp](../../kernel/layers/Logic/VirtualFileSystem.hpp).
- Walk allocated process/task roots, deduplicate shared tables/files, and bound
  descriptor scans (capacity can be large). Descriptor references and weak
  observers are different relationships. Observed references do not prove that
  all kernel references have been accounted for.

##### SynchronizationManager: futex keys, waiters and priority inheritance

- Question: what is a blocked task waiting on, and which recorded owner can make progress?
- Show private/shared futex key identity and offset, backing-object retention,
  waiter queue order/bits/sequence, wake/requeue/conversion state, PI owner,
  PI waiters, donated priority and requested reschedule CPUs. Correlate with tasks.
- Source: [SynchronizationManager.hpp](../../kernel/layers/Logic/SynchronizationManager.hpp)
  (`Waiters`, `Head`, `Tail`, `PiStates`, `PiWaiters`, `PriorityRescheduleMask`).
- Bounded slot arrays and index links support collection. Shared futex keys are
  not merely virtual addresses. A waiter/owner graph is evidence, not a general
  deadlock proof; ordinary byte spinlocks have no owner metadata.

##### SignalManager: pending delivery and handler state

- Question: why is a signal pending but not delivered, or why did a wait resume?
- Show process/task pending masks, task masks and deferred restoration, shared
  dispositions/actions, sender records, queued real-time signals, alternate stack,
  delivery depth and stopped/exit state. Distinguish process-directed from
  task-directed signals and queue reservations from published records.
- Sources: process/task fields, [SignalState.hpp](../../kernel/layers/Resource/SignalState.hpp)
  (`signal_queue_t`, bounded records/count/reserved),
  [SignalManager.hpp](../../kernel/layers/Logic/SignalManager.hpp),
  [signal_types.h](../../klib/signal_types.h).
- These are current delivery records; they do not retain every past handler invocation.

##### EventManager: epoll watch topology and notification queues

- Question: why does a consumer fail to wake or an edge/one-shot watch stop reporting?
- Show epoll instances, watched descriptors/files, masks/data, armed/pending state,
  sequence/topology generations and close observers. Include event-counter values,
  inotify watches and queued records/overflow, and signalfd masks.
- Sources: [EventManager.hpp](../../kernel/layers/Logic/EventManager.hpp)
  (`Epolls`, `Notifications`, generations),
  [EventManager.cpp](../../kernel/layers/Logic/EventManager.cpp)
  (`event_epoll_t`, `event_epoll_watch_t`, `event_counter_t`),
  [EventNotifications.cpp](../../kernel/layers/Logic/EventNotifications.cpp)
  (`event_notification_t`, `inotify_state_t`, `signal_descriptor_t`).
- Include explicit ring order/head/count and weak-file relationships. Inspect
  stored readiness and source state; never call Poll/WatchReadiness callbacks to
  synthesize a result. Private/anonymous-namespace types need verified exposure
  through the manager's debug header/record rather than fragile name assumptions.

##### SocketManager: UNIX endpoints, accept queues and messages

- Question: which peer or queue blocks connect/accept/send/receive, and where are passed fds retained?
- Show endpoint addresses/type/state, peers, pending accept chains, backlog,
  packet queue metadata, bytes/limits, shutdown/readiness flags, ancillary rights
  and credentials. Cross-link file identities to descriptor owners where reachable.
- Sources: [SocketManager.hpp](../../kernel/layers/Logic/SocketManager.hpp)
  (`Endpoints`, `EndpointCount`),
  [SocketManager.cpp](../../kernel/layers/Logic/SocketManager.cpp)
  (`socket_endpoint_t`, `socket_packet_t`, `socket_ancillary_t`).
- UNIX sockets are the implemented transport, not TCP/IP. Bound every nested
  chain and ancillary count; payload bytes are optional detail. Peer symmetry and
  observed reference discrepancies can be reported without claiming a full leak detector.

##### InterProcessCommunicationManager: pipes and memory files

- Question: is a pipe full, empty or missing an endpoint, and why is a memory-file operation rejected?
- Show reader/writer files, references, ring head/size/capacity, packet boundaries,
  readiness transitions, memory-file seals, backing pages and file size.
- Source: [InterProcessCommunicationManager.cpp](../../kernel/layers/Logic/InterProcessCommunicationManager.cpp)
  (`pipe_state_t`, `memory_file_t`, operation tables).
- There is no manager-wide object list. Discover through verified descriptor/file
  operation identities and privateData, or deliberately add a debug catalogue if
  complete detached-object enumeration becomes a requirement. Keep these two
  guarantees distinct. Full pipe payload is not a default summary.

##### VirtualMemoryManager / ProcessController: mappings and backing ownership

- Question: what retains a mapping/backing page, or why is a file-backed update not released?
- Show per-process address spaces, free/used regions and protection limits,
  shared/private mappings, backing offsets, objects/pages, dirty/shared accounting,
  reference/mapping counts, heap ownership and pending failed writeback objects.
- Sources: [vmm.h](../../kernel/platform/memory/vmm.h)
  (`virt_addr_space_t`, `virt_region_t`, `virt_backing_object_t`, `virt_shared_page_t`),
  [vmm.c](../../kernel/platform/memory/vmm.c) (`pending_writeback`),
  [VirtualMemoryManager.hpp](../../kernel/layers/Resource/VirtualMemoryManager.hpp).
- VMM is largely a facade over platform state. Expose roots explicitly; there is
  no established global catalogue of all backing objects. Reachable graphs and
  the pending-writeback list are not a complete physical-page ownership ledger.
- A related x86_64 page-table walk can explain selected VA translation/permissions
  using [arch_paging.c](../../arch/x86_64/arch_paging.c) and HHDM metadata. Bound by
  selected address/range; distinguish software mapping state from actual TLB contents.

##### PhysicalMemoryManager: NUMA topology, buddy lists and PFN detail

- Beyond the agreed maintained statistics, show each NUMA node's ordinary/DMA
  zones, physical ranges, per-order free blocks and largest available orders,
  plus a selected PFN's flags/order/list neighbours.
- Sources: [pmm.h](../../kernel/platform/memory/pmm.h)
  (`numa_node_t`, `zone_t`, `page_t`, buddy lists/metadata),
  [pmm.c](../../kernel/platform/memory/pmm.c), `platform.numa_nodes`.
- This explains fragmentation and failures despite apparently sufficient total
  free memory. Zone membership matters: DMA and ordinary zones share PFN metadata
  but own disjoint ranges/lists. Used pages do not directly give allocation count.
  Existing metadata does not identify the call site that allocated a block.

##### Platform heap: slab occupancy and object-address lookup

- Question: which size class/slab owns an address, and do its bitmap and counts agree?
- Show per-node caches, partial/full/empty slab lists, size classes, total/free
  objects, object arrays/bitmaps, and metadata chunk/free-list state.
- Sources: [heap.h](../../kernel/platform/memory/heap.h)
  (`kernel_heap_t`, `cache_t`, `slab_t`),
  [metadata.h](../../kernel/platform/memory/metadata.h), NUMA heap roots.
- Prefer an address/class/node selector over full bitmap dumps. Explain slot
  occupancy, not object type, stack trace or provenance; those require new tracking.

##### DmaManager: live and releasing allocation records

- Question: which device mapping retains pages, and are mask/alignment/boundary constraints satisfied?
- Show allocation ID, CPU/physical/DMA addresses, requested/reserved/mapped sizes,
  device constraints, mapping cookie, backend identity and releasing state.
- Sources: [DmaManager.hpp](../../kernel/layers/Resource/DmaManager.hpp)
  (`Allocations`, `AllocationCount`),
  [DmaManager.cpp](../../kernel/layers/Resource/DmaManager.cpp) (`AllocationRecord`).
- Include retained rollback/cleanup records and bounded links. A backend pointer
  or mapping cookie does not imply an inspectable IOMMU page table.

##### TimeManager: clock configuration and process timer relationships

- Question: which timer is armed, against what clock, and why might a timed wait not progress?
- Show clock/realtime availability, resolution, backend/source configuration and
  last sampled epoch; process real/CPU timer deadlines/intervals and CPU accounting;
  correlate tasks with the scheduler sleep entries and stored LAPIC calibration.
- Sources: [TimeManager.hpp](../../kernel/layers/Resource/TimeManager.hpp), process
  timer fields, [arch_time.c](../../arch/x86_64/arch_time.c)
  (`sources`, `active_source`), [clock.h](../../klib/clock.h), lapic.c.
- Stored samples are not a fresh wall-clock read. Do not read live MMIO/ports or
  invoke the clock backend during collection. CPU-time and monotonic deadlines
  must remain distinct.

##### CredentialManager: per-process security context

- Show real/effective/saved/filesystem UID/GID, supplementary groups, shared group
  references, cwd/umask and relevant inode ownership/mode. This helps explain
  permission failures after clone/exec or identity changes.
- Sources: [CredentialManager.hpp](../../kernel/layers/Logic/CredentialManager.hpp)
  and process credentials/groups in ProcessManager.hpp.
- CredentialManager has no separate identity database. Make this a focused
  process view with optional file comparison rather than invent a manager list.

#### Useful non-manager and cross-manager views

- **Self-test cases and failures:** SelfTestManager's active case, phase/context,
  result table, failure details, group stack and finalization state. Bounded state
  in [SelfTestManager.hpp](../../kernel/selftests/SelfTestManager.hpp); useful for
  hangs as well as failure counts.
- **Published boot diagnostics:** `boot_errors` in [klib.c](../../klib/klib.c),
  bounded by [error_logger.h](../../klib/error_logger.h). Preserve message order,
  ready flags, pending entries and capacity-exceeded counts. No timestamps, CPU
  attribution or automatic syscall-errno history exists in these records.
- **CPU progress/IPI matrix:** CPU address-space ownership, preemption/switching,
  pending reschedule, syscall-active state, IPI type/range/lock and interrupt
  configuration from [cpu.h](../../kernel/platform/cpu/cpu.h) and
  [x86_64.h](../../arch/x86_64/x86_64.h). This can explain stalled TLB/reschedule
  requests using RAM state, without pretending to read hardware TLB contents.
- **Observed resource-sharing graph:** correlate process/task tables, descriptor
  tables, files, sockets, event watches, mappings and backing pages by address.
  Build on the focused views; deduplicate objects, distinguish owning/weak links,
  cap depth/edges and label incomplete roots. Not a standalone proof of leaks.

#### Candidates that require new tracking or do not justify another rich view

- **ELF image/segment view:** ElfMapper defines `elf_metadata_t` and segments,
  but production exec keeps them in temporary `ExecImage` state, frees segment
  arrays on destruction, and sets `current->elfMetadata` to null. The non-null
  assignment found is in a self-test. A persistent image/segment or loaded-library
  catalogue requires deliberate debug-only retention/publication; do not promise
  it from existing per-process pointers. Sources:
  [ElfMapper.hpp](../../kernel/layers/Logic/ElfMapper.hpp),
  [ProcessLifecycle.cpp](../../kernel/layers/Logic/ProcessLifecycle.cpp).
- **Syscall/exception frames:** explicit valid frame-pointer decoding is feasible.
  An automatically discoverable frame table or historical call stream needs
  additional lifetime-safe tracking. Per-CPU syscall-active flags alone do not
  locate every valid frame. Historical tracing is outside this information pass.
- **Request managers:** inspected headers mostly store Resource/Logic/manager
  routing pointers. Their rich objects belong to the managers above; operation
  instrumentation, registration, stats headers and views for Request managers
  are explicitly out of scope by owner direction. InterruptRequestManager has
  a per-CPU ScheduleTickCounters array, but this does not override that exclusion.
  CPU/IPI progress views use platform/architecture and Resource/Logic state.
- **ResourceFileSystem:** its backend refers to InitRamFileSystemManager and
  path-based resource nodes; combine it with the agreed VFS/initramfs contents
  work rather than duplicate the same namespace in a separate command family.
- **General lock-owner/deadlock views:** raw spinlocks store no owners. Futex PI
  state supports a specific observed owner/waiter view; extending this to every
  lock would require instrumentation and different scope.

#### Suggested ordering and checks

1. Exercise the common interface with PMM/NUMA stats and InitRamFS's bounded array.
2. Migrate existing process/task/VMM/heap/VFS/CPU/PCI information; add descriptor
   ownership as the foundation for object-specific views.
3. Prioritize scheduler/synchronization, process retirement, mapping/backing,
   signals and event/socket/pipe relationships for investigation value.
4. Add DMA, timers, self-test/error records and optional cross-manager correlation
   according to agreed scope. ELF retained metadata needs a separate explicit choice.

For every selected view, specify record/root lifetime, entry/byte/depth limits,
ordering and count semantics, null/invalid/partial state reporting, and the
meaning of every cross-reference. Verify against relevant self-test fixtures and
controlled malformed data; no target method calls. Document how a future native
reader can recover the same information without Python or DWARF-driven kernel
reflection. This source assessment did not build or boot the kernel and does not
claim runtime verification of proposed registrations or adapters.
