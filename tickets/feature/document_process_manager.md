# Document ProcessManager

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Create an implementation-level reference for `ProcessManager` in the Resource layer.
Focus on process records, descriptor tables, filesystem context, credentials, sharing and resource lifetime.

Explain the real public interface and important internal helpers/data structures,
callers and collaborators, state transitions, ownership and cleanup, concurrency and
execution-context assumptions, errors and partial-failure paths. Trace representative
success and failure paths through named symbols. Cover implemented behavior and
limitations, with enough detail to support diagnosis and safe modification.
Use existing tests and debugging facilities as evidence and investigation entry points.

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

Coordinate with [the Resource layer guide](document_resource_layer.md). That ticket owns layer-wide wiring; this ticket owns `ProcessManager` internals.

## Source entry points

- [kernel/layers/Resource/ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp)
- [kernel/layers/Resource/ProcessManager.cpp](../../kernel/layers/Resource/ProcessManager.cpp)
- [kernel/selftests/resource/ProcessManagerTests.cpp](../../kernel/selftests/resource/ProcessManagerTests.cpp)

These are starting points; inspect callers, dependencies, and tests before making claims.

## Acceptance criteria

- [ ] Responsibilities, API contracts, callers/dependencies and source navigation are documented.
- [ ] Important structures and implementation paths, state transitions, ownership and teardown are explained.
- [ ] Locking/atomicity, ordering and interrupt/CPU/context constraints are documented where applicable; uncertainty is labeled.
- [ ] Representative success and failure walkthroughs identify concrete symbols and cross-layer handoffs.
- [ ] Relevant tests, coverage gaps, failure symptoms and useful debugger/log inspection points are described; commands are verified if supplied.
- [ ] Documentation links back to its layer guide and distinguishes implementation facts from inferred rationale.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned documentation work in the codebase understanding backlog.
