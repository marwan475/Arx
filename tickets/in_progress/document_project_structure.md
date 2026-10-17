# Document overall kernel architecture and responsibilities

- Type: feature
- Priority: P1
- Status: in progress
- Required skill: mattpocock-skills:grilling

## Task prompt

Write a human-readable explanation of Arx's overall kernel architectural design in
`docs/architecture/KernelArch.md` (corrected directory spelling). Explain
arch, platform, klib, Resource, Logic, Request, and Dispatcher: their responsibilities,
boundaries, and interactions. Include every manager in each formal layer with a
one-line responsibility, plus the other principal components constructed by its
factory. Organize the guide by initialization order. Use concise, human-readable prose
and plain filenames only, without paths or clickable links.

The owner's design rules are: arch contains architecture-specific code for
portability; platform is the hardware interface; klib is the shared kernel standard
library available to every kernel area, including future drivers. Higher layers may
access any lower layer, while lower layers cannot access layers above them.
Dispatcher is how platform communicates with the layer stack. Explain these rules
and distinguish any verified implementation differences from the intended design.

Keep this focused on kernel design rather than repository structure. Leave build
instructions, detailed subsystem walkthroughs, and operational/debugging manuals
to other guides. Leave the root README and existing diagrams unchanged. Diagrams
are deferred; track that follow-up as a TODO in this ticket.

Write for an experienced kernel developer learning Arx's implementation, and for
agents gathering focused context or investigating failures. Explain repository-specific
mechanics and rationale supported by the code; distinguish verified behavior from
inferred intent, limitations, and unknowns. Avoid generic kernel tutorials.

Follow [the ticket workflow](../TICKET.MD). During analysis use
`mattpocock-skills:grilling` to agree documentation placement, depth, diagrams, and
examples. Record the plan, set status to `analyzed`, and wait for the owner's
explicit execution instruction. This ticket authorizes documentation work; record
implementation defects discovered during research separately.

## Source entry points

- [README.md](../../README.md)
- [docs/DIAGRAMS.md](../../docs/DIAGRAMS.md)
- [arch/arch.h](../../arch/arch.h)
- [kernel/bootstrap.c](../../kernel/bootstrap.c)
- [kernel/kernel.cpp](../../kernel/kernel.cpp)
- [kernel/layers/Dispatcher.cpp](../../kernel/layers/Dispatcher.cpp)
- [kernel/layers/Resource/ResourceLayerFactory.hpp](../../kernel/layers/Resource/ResourceLayerFactory.hpp)
- [kernel/layers/Logic/LogicLayerFactory.hpp](../../kernel/layers/Logic/LogicLayerFactory.hpp)
- [kernel/layers/Request/RequestLayerFactory.hpp](../../kernel/layers/Request/RequestLayerFactory.hpp)
- [klib](../../klib)

These are starting points; inspect callers, dependencies, and tests before making claims.

## Acceptance criteria

- [x] `docs/architecture/KernelArch.md` explains the overall kernel design in human-readable prose.
- [x] Arch, platform, klib, Resource, Logic, Request, and Dispatcher responsibilities and interactions follow the agreed design; verified implementation differences are explicit.
- [x] Every manager in each formal layer has a one-line responsibility; other principal factory-created components are included.
- [x] Dependency direction, factory/capability wiring, and initialization order are explained with plain filenames and no clickable links.
- [x] Root README and existing diagrams remain unchanged by this ticket; diagram work is recorded as a deferred TODO.

## Deferred TODO

- Add architecture diagrams in later work, after agreeing their scope with the owner.
  This follow-up is outside the current completion criteria.

## Work log

### 2026-10-17 — Created

Assigned P1 because the project map establishes navigation and boundaries for the remaining documentation.

### 2026-10-17 — Analysis started; owner decisions pending

Read `tickets/TICKET.MD` and the required `mattpocock-skills:grilling` skill.
The owner's request identifies this overall architecture ticket rather than the
separate architecture-specific implementation guide. Status remains `open` until
the grilling conversation and shared-understanding confirmation are complete.

Initial source inspection confirms that README's bootstrap names are stale:
`kernel/bootstrap.c` defines `platform_init` and `platform_init_complete`.
`Dispatcher::StartKernel` constructs Resource, runs Resource tests, constructs
Logic, runs Logic tests, then constructs Request. Request's factory explicitly
accepts both Resource and Logic capabilities, contradicting README's strict
adjacent-layer-only description. These are documentation discrepancies to
reconcile, not implementation changes authorized by this ticket.

Proposed decisions submitted to the owner:

- Place the detailed overview in `docs/ARCHITECTURE.md`, linked from README and
  the diagram index.
- Cover ownership, actual dependencies, boot through scheduling, build artifacts,
  and change/debugging navigation; use boot and one runtime request walkthrough.
  Leave subsystem internals and detailed operating instructions to their tickets.
- Use source-linked Mermaid diagrams for boundaries and handoffs; correct or
  label stale existing diagrams while preserving useful images.

Next: resolve these decisions, finish source-backed analysis and verification
planning, obtain confirmation of shared understanding, record **Analysis and
plan**, and set status to `analyzed`. Execution then requires the owner's explicit
instruction under the ticket workflow.

### 2026-10-17 — Analysis and plan

The owner resolved the grilling questions and extended the agreed scope to include
all managers with a one-line responsibility under their respective layers. This
entry supersedes the initial proposed placement, repository overview, README
navigation, and diagram work above. Preserve the ticket filename for continuity.

Agreed output: `docs/architecture/KernelArch.md`, explaining kernel design and
responsibilities using readable prose, selective source links, a brief initialization
overview, and concise manager inventories. Use the owner's architecture rules in
the revised task prompt. No README edits or diagrams; the deferred TODO is recorded.

Source findings: `Dispatcher::StartKernel` constructs Resource, then Logic with
Resource capabilities, then Request with both Resource and Logic capabilities.
This supports access to any lower layer rather than the stale adjacent-only claim
in README. `platform_init` and `platform_init_complete` are the current bootstrap
symbols. Factory construction provides the inventory baseline: eight Resource
components, ten Logic components, and twelve Request managers. Include Scheduler,
ProcessController, ElfMapper, VirtualFileSystem, and ResourceFileSystem even though
their names do not end in Manager. Verify each responsibility against its interface
and implementation during execution.

Plan:

1. Inspect the architecture contract, platform services, klib interfaces,
   bootstrap, Dispatcher, factories, and manager interfaces at orientation depth.
2. Write the agreed guide, explaining the hardware boundary, shared utilities,
   layer responsibilities, downward dependencies, and Dispatcher entry bridge.
3. Add manager/component tables grouped by layer with one-line responsibilities
   and source links; summarize initialization without deep subsystem internals.
4. Check every inventory against factory construction and every link against the
   filesystem. Verify dependency and startup claims against concrete callers.
   Inspect the final diff to ensure only the guide and ticket bookkeeping change.

Risks: intended design and current implementation may differ, especially where
architecture hooks implement hardware operations or callbacks bridge boundaries.
Describe such differences explicitly without changing implementation. Future
drivers' use of klib is a design allowance, not a claim that those drivers exist.
No runtime tests are required for prose-only changes; source, inventory, and link
checks are the required verification. No scope decisions remain unresolved.

Status is `analyzed`; execution awaits the owner's explicit instruction under
`tickets/TICKET.MD`. On execution, move this ticket to `tickets/in_progress/`.
If unfinished, leave it there with a progress entry; on completion, close it and
update the feature and closed logs according to the guide.


### 2026-10-17 — Execution started

Owner explicitly authorized execution of the recorded plan. Moved this ticket
to `tickets/in_progress/`; documentation research and writing are underway.

### 2026-10-17 — Verification and completion

Created `docs/architecture/KernelArch.md` with readable explanations of arch,
platform, klib, all three layers, Dispatcher, dependency rules, and initialization.
Included one-line responsibilities for all eight Resource components, ten Logic
components, and twelve Request managers; distinguished intended boundaries from
verified architecture-hook and callback integration paths.

Verification: all 61 local guide links resolve; each of the 30 components
constructed by the three factories has a linked responsibility row. Reviewed
startup and dependency claims against bootstrap, kernel startup, Dispatcher,
factory implementations, and manager interfaces; checked callback registration
and AArch64 support gaps against implementation. Whitespace and absence-of-diagram
checks passed. This execution changed only the new guide and ticket bookkeeping;
the pre-existing README edits and diagram moves were left untouched. No build or
runtime tests were needed for this documentation-only change.

All current acceptance criteria are met. Diagrams remain a deferred follow-up,
as requested. Moved the ticket to `tickets/closed/` and appended its filename to
the closed and originating feature logs.

### 2026-10-17 — Premature closure corrected; awaiting owner review

The owner clarified that execution authorization did not authorize ticket closure.
The preceding closure was premature. Restored status to `in progress`, returned
this ticket to `tickets/in_progress/`, and removed the premature closure entry
from each closure log. The guide and its verification remain available for review;
diagrams remain the deferred TODO above. No new documentation work was performed.

Next action: await the owner's review, explicit authorization to close, and
instruction confirming where the ticket should move next. Updated
`tickets/TICKET.MD` to require this gate for all future agents.

### 2026-10-17 — Documentation directory spelling corrected

At the owner's instruction, moved the guide to `docs/architecture/KernelArch.md`
and removed the now-empty misspelled directory. Updated ticket path references;
all 61 guide source links still resolve. Status remains `in progress`, awaiting
owner review and closure/destination authorization.

### 2026-10-17 — Human-readable revision; ready for owner review

At the owner's request, shortened the guide and organized it by initialization:
Arch entry, Platform, Klib availability, Dispatcher, Resource, Logic, Request, and
scheduled kernel work. Klib has no single initialization step; early helpers and
allocation prerequisites are distinguished. Replaced links with plain filenames,
shortened every component summary, and retained all 30 factory-created components.
This presentation requirement supersedes earlier plans for source links.

Verification: every cited filename exists; all eight Resource, ten Logic, and
twelve Request components remain in the tables. Confirmed no clickable links or
relative file paths remain in the guide, and checked whitespace. No code changed.
Status remains `in progress`; awaiting owner review and explicit authorization
before closing or moving the ticket. The diagrams TODO remains deferred.

### 2026-10-17 — Self-test overview added; ready for owner review

At the owner's request, added a concise self-tests section after scheduled startup.
It explains purpose, initialization phases, bootstrap/worker responsibilities,
framework filenames, and log outcomes. Checked phase order and final-summary
behavior against `SelfTestManager.cpp` and its interface. Plain filenames and
no-link formatting checks pass. Ticket remains `in progress`, awaiting owner
review and explicit closure/destination authorization; diagrams remain deferred.
