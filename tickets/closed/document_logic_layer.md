# Document Logic layer

- Type: feature
- Priority: P2
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Create a detailed guide to Logic layer. Explain LogicLayerFactory/LogicLayerCaps, construction and cross-component wiring, and how Resource mechanisms support policy. Map scheduling, process control/memory, VFS, ELF, credentials, events, synchronization, signals, sockets and IPC. Describe general cross-manager interactions and identify ownership/synchronization boundaries; concrete examples and walkthroughs are deferred by owner agreement.

Keep this guide focused on area-wide contracts and interactions; link component guides for internal detail.

Write concise, human-readable documentation for humans learning Arx's implementation. Explain repository-specific
mechanics and rationale supported by the code; distinguish verified behavior from
inferred intent, limitations, and unknowns. Avoid generic kernel tutorials.

Follow [the ticket workflow](../TICKET.MD). During analysis use
`mattpocock-skills:grilling` to agree documentation placement, depth, diagrams, and
examples. Record the plan, set status to `analyzed`, and wait for the owner's
explicit execution instruction. This ticket authorizes documentation work; record
implementation defects discovered during research separately.

## Related work

Coordinate structure and navigation with [the project overview](../closed/document_project_structure.md). Formal layer guides own factory/capability explanations; component tickets own component internals.

## Source entry points

- [kernel/layers/Logic](../../kernel/layers/Logic)
- [kernel/layers/Dispatcher.cpp](../../kernel/layers/Dispatcher.cpp)
- [kernel/selftests/logic](../../kernel/selftests/logic)

These are starting points; inspect callers, dependencies, and tests before making claims.

## Acceptance criteria

- [x] Responsibilities, source map, public contracts, initialization order and dependencies are documented.
- [x] Key structures, ownership/lifetimes, synchronization and execution-context constraints are explained with source evidence.
- [x] General interactions connect the layer to its collaborators; concrete examples and walkthroughs are deferred.
- [x] Failure modes, useful inspection locations, relevant self-tests, and existing debugging tools are described using plain source filenames, without clickable source links.
- [x] Current limitations and architecture differences are explicit; documentation navigation and image paths resolve.

- [x] `docs/architecture/layers/Logic.md` follows the established concise, initialization-first format, with a complete one-line component inventory.
- [x] Initialization and general interaction SVGs are embedded; Mermaid sources remain in `docs/DIAGRAMS.md`.
- [x] The root README links to the finished guide.

## Deferred TODOs

- Add concrete examples and walkthroughs in a later owner-approved update. These
  are not required for completion of this ticket.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned documentation work in the codebase understanding backlog.

### 2026-10-17 — Analysis and plan

Used `mattpocock-skills:grilling` and
`docs/skills/write-kernel-docs/SKILL.md`. The owner confirmed analyzing Resource
and Logic together, with separate `Logic.md` guides beside Request and Dispatcher,
README links, concise human-readable prose, initialization-first structure,
one-line component responsibilities, ownership/contracts, limitations, and
self-tests. Each guide will have initialization and general interaction SVGs with
retained Mermaid sources. Concrete examples and walkthroughs are explicitly
deferred; the original walkthrough criterion is superseded by that agreement.

Inspected both factories and capability headers, `ResourceFileSystem.hpp`,
Dispatcher construction/test calls, SelfTestManager test-phase wiring, and the
Resource/Logic self-test inventories.

The factory exports ten components: Scheduler, VirtualFileSystem, ElfMapper,
CredentialManager, EventManager, SynchronizationManager, SignalManager,
ProcessController, SocketManager, and InterProcessCommunicationManager. Create
requires Resource capabilities and returns null if they are absent. EventManager
is wired back into Scheduler and VirtualFileSystem; SynchronizationManager is
registered as Scheduler's priority manager. ProcessController receives both
Resource and Logic capabilities. Socket and IPC receive VFS, Event, and Signal
services. Repeated Create calls return existing caps. Dispatcher runs Logic tests
before constructing Request.

Plan:

1. Explain all ten exported components with one-line responsibilities, construction
and cross-component wiring, Resource imports, policy/mechanism boundaries,
process-memory ownership within process control, and interactions between
scheduling, VFS/ELF, credentials, events, synchronization, signals, sockets, and IPC.
Inspect implementations, callers, and tests for actual lifetimes, locking,
execution constraints, errors, and support limits before writing claims.
2. Write `docs/architecture/layers/Logic.md` using the agreed format. Keep
   component internals in their dedicated guides, and use plain source filenames
   without clickable source links. Coordinate terminology with the companion layer
   guide and avoid repeating Dispatcher or platform explanations.
3. Create two SVGs for initialization and general interactions, using the existing
   diagram style; retain Mermaid sources in `docs/DIAGRAMS.md` and visually inspect
   both rendered diagrams. Include no concrete examples or walkthroughs.
4. Add README navigation. Verify claims against source, the complete factory
   inventory, filenames, local links/images, SVG metadata/rendering, and whitespace.
   This documentation-only work requires no kernel build or boot.

Risks: capability pointers do not establish ownership or isolation by themselves;
factory destructors are not proof of a coordinated running-kernel shutdown.
Platform/architecture stubs and test presence must not be described as completed
runtime support. Verify these distinctions in the guide, and record discovered
implementation defects separately without changing code. No scope questions remain.

Status is analyzed; awaiting the owner's explicit execution instruction for both
tickets. On execution, move the entire ticket to `tickets/in_progress/`. Finished
work remains there until the owner explicitly authorizes closure and destination.

### 2026-10-17 — Execution started

The owner authorized execution of both layer documentation tickets with “go ahead”.
Moved this entire ticket from feature to in_progress. Writing the agreed separate
guide, two SVGs, and README navigation. Concrete examples remain deferred.

### 2026-10-17 — Ready for owner review

Created `docs/architecture/layers/Logic.md` in the agreed concise,
initialization-first format. Covers the complete component inventory,
capabilities/dependencies, ownership and lifetime, general interactions,
synchronization/execution constraints, failures, support limits, and self-tests.
Added two embedded SVGs, retained their Mermaid sources in `docs/DIAGRAMS.md`,
and added the root README link. Concrete examples and walkthroughs remain deferred.

Verified claims against both factories, Resource component contracts and relevant
implementations, Logic construction/callback wiring and collaborators, Dispatcher,
architecture support boundaries, and self-test group wiring. Confirmed all
10 component rows and corresponding self-test suites, 22 explicit filename
references, and local document/image/ticket links after moving the ticket.
Parsed the SVGs, verified accessibility metadata and preserved native text spacing,
and confirmed retained Mermaid sources exactly match render inputs. Rendered and
visually inspected both SVGs for readable text, clipping, spacing, and arrows.
`git diff --check` passed. README retains the original architecture image and
omits skill/AArch64 guide links as requested. No implementation files changed;
no kernel build or runtime self-tests were run for this documentation-only work.

All agreed acceptance criteria are met. Status remains in progress while awaiting
owner review and explicit closure/destination authorization. Proposed destination:
`tickets/closed/document_logic_layer.md`. Closure logs are unchanged for this ticket.

### 2026-10-17 — Completion

The owner authorized closure with “close” in response to the proposed move to
`tickets/closed/`. Completed the layer guide, two verified SVGs with retained
Mermaid sources, self-test documentation, and README navigation. Concrete
examples and walkthroughs remain deferred as agreed.
