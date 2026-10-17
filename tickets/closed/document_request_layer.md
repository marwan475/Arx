# Document Request layer

- Type: feature
- Priority: P2
- Status: closed
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

Keep this guide focused on area-wide contracts and interactions; link component guides for internal detail.

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

- [x] Responsibilities, source map, public contracts, initialization order and dependencies are documented.
- [x] Key structures, ownership/lifetimes, synchronization and execution-context constraints are explained with source evidence.
- [x] General request flow connects entry points to collaborators without concrete examples or walkthroughs.
- [x] Failure modes, useful inspection locations, relevant self-tests, and existing debugging tools are described using plain source filenames, without clickable source links.
- [x] Current limitations and architecture differences are explicit; documentation navigation and image paths resolve.
- [x] Initialization and general request-flow SVGs are embedded, with editable Mermaid sources retained in `docs/DIAGRAMS.md`.
- [x] The root README links to the finished guide.

## Deferred TODOs

- Add concrete request examples and walkthroughs in a later, owner-approved update;
  these are not required for completion of this ticket.

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
