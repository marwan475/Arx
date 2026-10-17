# Document architecture-specific code

- Type: feature
- Priority: P2
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Create a detailed guide to architecture-specific code. Map the common architecture contract to x86_64 and AArch64 implementations. Cover entry/initialization, CPU and interrupt state, paging/TLB operations, context switching, syscall entry/return, signals, timing and DMA hooks where implemented. Identify architecture-specific layouts and calling conventions, and verify support gaps rather than assuming parity.

Create separate `docs/architecture/arch/x86_64.md` and
`docs/architecture/arch/aarch64.md` guides. Keep the established concise,
human-readable format, with plain source filenames and no clickable source links.
Clearly separate implemented behavior, partial support, and stubs. Organize x86_64
by initialization and subsystem, explaining purpose, current behavior, key
structures/layouts, calling conventions, collaborators, and limitations. Document
AArch64's existing code and unsupported operations without implementing it.
Add SVG initialization and interrupt/syscall entry-return diagrams, retaining
editable Mermaid sources, and add links to both guides in README.

Write for an experienced kernel developer learning Arx's implementation, and for
agents gathering focused context or investigating failures. Explain repository-specific
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

- [arch/arch.h](../../arch/arch.h)
- [arch/x86_64](../../arch/x86_64)
- [arch/aarch64](../../arch/aarch64)
- [kernel/bootstrap.c](../../kernel/bootstrap.c)

These are starting points; inspect callers, dependencies, and tests before making claims.

## Acceptance criteria

- [x] Responsibilities, source map, public contracts, initialization order and dependencies are documented.
- [x] Key structures, ownership/lifetimes, synchronization and execution-context constraints are explained with source evidence.
- [x] At least one concrete control/data-flow walkthrough connects entry points to collaborators.
- [x] Failure modes, useful inspection points, relevant tests, and existing debugging tools are identified with plain filenames.
- [x] Current limitations and architecture differences are explicit; source filenames exist, SVG embeds resolve, and README links to both separate architecture guides.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned documentation work in the codebase understanding backlog.

### 2026-10-17 — Analysis and plan; execution authorized

Completed the required grilling conversation. The owner requested separate docs
for each architecture, structured details for implemented x86_64 areas, and SVG
initialization and entry/return diagrams with Mermaid sources. AArch64 must stay
stubbed. After confirming the filenames and README navigation, the owner explicitly
instructed “good to go”; this authorizes executing the agreed plan.

Plan: trace arch.h and each target's entry, initialization, paging, context,
interrupt, syscall, signal, time, DMA, and firmware/device code with their callers.
Write x86_64.md by subsystem with concise behavior and structure details. Write
aarch64.md as a separated existing-code/stub inventory, distinguishing implemented
paging code from unavailable runtime support. Add source-backed SVG flow diagrams,
retain Mermaid in DIAGRAMS.md, and link the guides from README.

Initial findings: AArch64 has substantial paging code and basic entry/stack helpers,
but arch_init and task-context support return false and user-mode entry panics.
Its structures do not establish a working syscall or scheduling implementation.
Verify each operation separately rather than labeling the entire target implemented
or assuming every file is a stub.

Verification: check source filenames and symbol/layout claims against C/C++ and
assembly, validate support tables against actual function bodies, render and
visually inspect SVGs, and verify README/image links. Confirm no implementation
files changed. Runtime tests are not required for documentation-only edits and
must not be implied by source inspection. Unknown or unverified runtime behavior
will be labeled. Record any discovered implementation defects separately.

Moved to tickets/in_progress when execution began. Keep status in progress for
owner review after verification; closure and destination require owner approval.

### 2026-10-17 — Ready for owner review

Created separate x86_64.md and aarch64.md guides in docs/architecture/arch, using
plain source filenames, concise subsystem sections, and no clickable source links.
The x86_64 guide covers boot/per-CPU initialization, structures and ABI offsets,
paging/TLB and root lifetimes, interrupt/syscall entry and return, task/TLS and
floating-point state, signal frames, clocks, timers, DMA, limitations, and useful
test/debugging files. The AArch64 guide separates existing boot/stack/paging code
from unsupported runtime hooks and defined-but-unimplemented structures.

Added four Mermaid-generated SVGs: x86_64 initialization, interrupts, syscalls,
and the AArch64 initialization boundary. Kept editable sources in DIAGRAMS.md and
added README navigation to both guides, preserving the original README image.

Verification passed:

- Source filenames exist; README links and all four SVG embeds resolve.
- SVGs parse as native vector/text documents with accessible titles/descriptions;
  rendered and visually inspected all four for readable labels, layout, and clipping.
- Reviewed syscall frame size and offsets, context offsets, register conventions,
  signal structure sizes, and return instructions against headers, static assertions,
  and assembly. Checked bootstrap ordering, huge-page splitting/range failure
  behavior, TLB targeting, interrupt guards, clocks, and DMA against implementations
  and callers. Checked AArch64 support rows against function bodies rather than
  inferring support from declarations or test filenames.
- Whitespace checks pass; no implementation files changed. No builds or runtime
  tests were run for these documentation-only changes; source presence is not
  presented as a successful AArch64 boot or a new hardware validation result.

All current acceptance criteria are met. Status remains in progress in
`tickets/in_progress/`, awaiting owner review, explicit authorization to close,
and confirmation of the next destination. AArch64 remains unchanged and stubbed.

### 2026-10-17 — Completion

The owner explicitly authorized closing this ticket and moving it to
`tickets/closed/` after review. Completed both architecture guides, four verified
SVG diagrams, and README navigation; AArch64 implementation remains unchanged.
Updated the originating feature log and the closed log.
