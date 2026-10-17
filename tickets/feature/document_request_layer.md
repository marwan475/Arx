# Document Request layer

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Create a detailed guide to Request layer. Explain RequestLayerFactory/RequestLayerCaps and Dispatcher syscall/interrupt routing. Map request managers, ABI decoding/validation, user-memory handling, delegation and result/error translation. Trace representative requests into Logic/Resource and architecture return paths; distinguish implemented behavior and unsupported paths.

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

- [kernel/layers/Request](../../kernel/layers/Request)
- [kernel/layers/Dispatcher.cpp](../../kernel/layers/Dispatcher.cpp)
- [kernel/selftests/request](../../kernel/selftests/request)

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
