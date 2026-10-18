# Document Request layer

- Type: feature
- Priority: P2
- Status: in progress
- Required skill: mattpocock-skills:grilling

## Task prompt

Create a concise, human-readable guide to the Request layer at
`docs/architecture/layers/Request.md`, following the format established in
`KernelArch.md` and `Dispatcher.md`. Explain RequestLayerFactory/RequestLayerCaps
and Dispatcher syscall/interrupt routing. Map request managers, ABI
decoding/validation, user-memory handling, delegation and result/error translation.
Describe the general flow into Logic/Resource and architecture return paths;
distinguish implemented behavior and unsupported paths. Concrete examples and
request walkthroughs are deferred at the owner's request.

Keep layer-wide contracts and interactions, and document every manager/component in its own section within this guide.

Write for humans learning Arx's implementation. Explain repository-specific
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

- [kernel/layers/Request](../../kernel/layers/Request)
- [kernel/layers/Dispatcher.cpp](../../kernel/layers/Dispatcher.cpp)
- [kernel/selftests/request](../../kernel/selftests/request)

These are starting points; inspect callers, dependencies, and tests before making claims.

## Acceptance criteria

- [x] Preserve the concise, human-readable, initialization-first guide and existing layer-wide contracts.
- [x] Include a separate section for every factory-created manager/component.
- [x] Each Request manager section identifies the Linux syscall family it handles; do not enumerate handler APIs. Describe Interrupt separately as a common kernel request handler.
- [x] Verify API descriptions against public headers and implementations, including overloads and cleanup obligations.
- [x] Remove layer diagram embeds, SVG assets, and their Mermaid source blocks for now.
- [x] Preserve self-test coverage descriptions, limitations, plain source filenames, and working README navigation.

## Deferred TODOs

- Reintroduce layer diagrams later, after the expanded component/API documentation is settled.
- Concrete examples and walkthroughs remain deferred.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned documentation work in the codebase understanding backlog.

### 2026-10-17 — Analysis and plan

Used `mattpocock-skills:grilling` and
`docs/skills/write-kernel-docs/SKILL.md`. The owner confirmed placement beside
Dispatcher, the established concise human-readable format, README navigation,
and initialization/request-flow SVGs. The owner explicitly excluded examples for
now; the original walkthrough criterion is replaced by a general flow explanation.

Inspected `RequestLayerFactory.hpp`, `RequestLayerFactory.cpp`, the request
self-test inventory, and README navigation. The factory creates twelve managers,
owns their exported capability pointers, and returns its existing capabilities on
subsequent Create calls. Dependencies differ: Memory receives Logic capabilities,
System receives Resource capabilities, and Interrupt takes neither constructor
argument; the remaining managers receive both. Runtime contracts still require
verification against manager bodies, Dispatcher, lower-layer collaborators, and
architecture entry/return code before being documented.

Plan:

1. Verify initialization, ownership/lifetimes, ABI validation, user-memory access,
   delegation, synchronization/execution constraints, errors, and support limits
   against implementation and relevant tests. Record discovered implementation
   defects separately; make documentation changes only.
2. Write `Request.md` in initialization-then-runtime order, with a compact inventory
   of all twelve managers and one-line responsibilities, key structures/contracts,
   limitations, and a self-tests section. Leave manager internals to their own
   guides. Use plain source filenames and no clickable source links.
3. Create two general SVGs covering initialization and request flow, retain their
   Mermaid sources, and visually check them in the existing diagram style.
   Do not include concrete syscall examples or walkthroughs.
4. Add the README guide link. Verify claims, manager inventory, document/image
   paths, SVG rendering, and whitespace. No kernel build or boot is required for
   documentation-only changes.

Risks: shared syscall constants do not establish full ABI or architecture support;
test presence does not prove a successful runtime test. Verify function bodies and
describe only supported behavior. No remaining scope questions.

Status is analyzed. Await the owner's explicit execution instruction before
moving the ticket to `tickets/in_progress/` and writing the guide. When ready for
review, keep it in progress until closure and destination are explicitly authorized.

### 2026-10-17 — Execution started

The owner authorized execution with “go”. Moved this ticket from feature to
in_progress, preserving its filename and history. Implementing the agreed guide,
two SVGs, and README navigation; concrete examples remain deferred.

### 2026-10-17 — Ready for owner review

Created `docs/architecture/layers/Request.md` with initialization, twelve manager
responsibilities, capabilities and lifetimes, general syscall flow, user-copy and
synchronization rules, interrupt requests, failure conventions, architecture
limits, and self-tests. Added README navigation, two embedded SVGs, and their
editable Mermaid sources in `docs/DIAGRAMS.md`. Concrete examples and walkthroughs
remain an explicit deferred TODO, as agreed.

Verified behavior against the Request factory and handlers, Dispatcher, Klib copy
helpers, ProcessManager descriptor guards, x86_64 syscall entry/return, AArch64
stubs, and self-test wiring/Makefile sources. Confirmed all twelve manager rows,
26 explicit source filename references, document/image links (including ticket
links after its move), SVG XML accessibility metadata and preserved text spacing,
and exact agreement between the rendered Mermaid inputs and retained sources.
Rendered and visually inspected both diagrams for readable labels, spacing,
clipping, and routing. `git diff --check` passed. No implementation files changed;
no kernel build or runtime self-tests were run for this documentation-only work.

All agreed acceptance criteria are met. Status remains in progress; awaiting the
owner's explicit authorization to close and move to
`tickets/closed/document_request_layer.md`. Closure logs have not been changed.

### 2026-10-17 — Completion

The owner authorized closure in response to the proposed move to
`tickets/closed/document_request_layer.md`. Completed the Request guide, two
verified SVG diagrams with retained Mermaid sources, and README navigation.
Concrete examples remain deferred as agreed.

### 2026-10-17 — Reopened; owner-directed scope consolidation

The owner explicitly requested reopening the layer tickets directly into in_progress,
removing the separate manager/component tickets, removing layer diagrams for now,
and expanding each layer guide with a section per manager/component. Resource and
Logic sections must describe every public API function; Request sections describe
Linux syscall families instead of listing their long handler APIs. This instruction
authorizes the revised work and the requested status/destination directly.

Updated plan: inventory public headers and verify implementations; add concise
component sections and per-function API tables (Request family summaries only);
remove the layer diagrams and their source blocks; check API/component coverage,
filenames, navigation, and whitespace. Preserve existing guide content where useful,
self-test sections, source filename style, and earlier work-log history. Record
new diagrams and examples as deferred, not completion requirements. Keep tickets
in progress for owner review; do not re-close without authorization.

Consolidated and removed these separate tickets:
- `document_credential_request_manager.md`
- `document_event_request_manager.md`
- `document_interrupt_request_manager.md`
- `document_memory_request_manager.md`
- `document_process_request_manager.md`
- `document_scheduler_request_manager.md`
- `document_signal_request_manager.md`
- `document_socket_request_manager.md`
- `document_sync_request_manager.md`
- `document_system_request_manager.md`
- `document_time_request_manager.md`
- `document_vfs_request_manager.md`

### 2026-10-17 — Progress and remaining work; revised guide ready for review

Twelve manager sections describing eleven Linux syscall families and the separate common interrupt handler; factory API described without enumerating manager handler functions. Retained initialization order, ownership/locking rules, limitations,
and self-test descriptions. Individual manager/component tickets are consolidated
into this guide and removed. Removed all eight SVGs embedded across the layer
guides (including Dispatcher), their embeds and exact Mermaid source blocks;
retained kernel, platform, and architecture diagrams and the original README image.

Verification: compared factory inventories to all 30 component/manager headings;
checked public-header declarations against their own component API sections,
including overload counts and excluding deleted copy operations. Reviewed relevant
implementation bodies for semantics, failure/cleanup obligations, and supported
paths. Verified public helpers and VFS callbacks, cited source filenames, README
navigation, and local guide/ticket links. Confirmed all 30 removed ticket files
are absent, all three layer tickets are in_progress, and no removed layer SVGs or
source blocks remain. `git diff --check` passed. No implementation files changed;
no kernel build/runtime tests were run for this documentation-only revision.

All revised acceptance criteria are met. Per the owner's instruction, this ticket
remains in progress; owner review is pending. Diagrams and concrete examples remain
explicit deferred TODOs. Historical closure log entries are preserved as history
of the previously authorized closure; this reopening adds no closure entries.
