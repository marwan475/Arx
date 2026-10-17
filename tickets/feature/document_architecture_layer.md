# Document architecture-specific code

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Create a detailed guide to architecture-specific code. Map the common architecture contract to x86_64 and AArch64 implementations. Cover entry/initialization, CPU and interrupt state, paging/TLB operations, context switching, syscall entry/return, signals, timing and DMA hooks where implemented. Identify architecture-specific layouts and calling conventions, and verify support gaps rather than assuming parity.

Keep this guide focused on area-wide contracts and interactions; link component guides for internal detail.

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

- [ ] Responsibilities, source map, public contracts, initialization order and dependencies are documented.
- [ ] Key structures, ownership/lifetimes, synchronization and execution-context constraints are explained with source evidence.
- [ ] At least one concrete control/data-flow walkthrough connects entry points to collaborators.
- [ ] Failure modes, useful inspection locations, relevant tests, and existing debugging tools are linked.
- [ ] Current limitations and architecture differences are explicit; code links resolve and layer/component documentation is cross-linked.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned documentation work in the codebase understanding backlog.
