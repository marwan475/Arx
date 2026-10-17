# Identify Arx agent skills and create implementation tickets

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Identify repository-specific agent skills that would help agents work on Arx,
gather relevant context, and investigate kernel failures. Produce one actionable
feature ticket per selected skill. Focus on repeatable workflows and the tools
agents actually have available, grounded in the current repository.

Follow [the ticket workflow](../TICKET.MD). During analysis use
`mattpocock-skills:grilling` to agree recurring tasks, skill boundaries, invocation
triggers, priorities, and repository placement/discovery. Record an analysis and
plan, set status to `analyzed`, and wait for the owner's explicit execution
instruction before conducting the full assessment and creating child tickets.

The owner is an experienced kernel developer. Skills should direct agents through
Arx-specific procedures, decisions, and verification, with links to detailed
documentation. Distinguish executable workflows from reference material; reuse
existing generic skills and project guides where appropriate.

### Required candidates to assess

1. **QEMU/GDB kernel debugging:** teach agents how to build a suitable kernel,
   launch or attach to QEMU, load matching symbols and the Arx GDB extension,
   inspect state, and collect useful evidence. Explain available launch options,
   GDB commands, Arx extension commands, their arguments, what their output means,
   when to use them, and their limitations. Include boot-phase and SMP/thread
   considerations, architecture differences, common failure modes, and session
   cleanup. Verify headless and noninteractive workflows suitable for agents.
   Explain how this complements `mattpocock-skills:diagnosing-bugs` rather than
   duplicating its general investigation method.
2. **Working with and adding self-tests:** teach agents to select the correct
   suite and execution phase, use existing fixtures and accounting conventions,
   add/register cases, build/run the relevant configuration, interpret results,
   and investigate failures or hangs. Cover pre-scheduler versus scheduled/SMP
   requirements, resource cleanup, userspace fixtures where relevant, and
   PASS/FAIL/SKIP/BLOCKED semantics. Reuse the self-test documentation as reference.

These examples must be evaluated and result in tickets unless the owner explicitly
agrees they should be deferred or combined. Identify additional skills from
recurring work supported by evidence in the code, scripts, tests, and project
workflow. Potential areas include build/boot reproduction, subsystem investigation,
architecture-specific changes, and cross-layer changes; assess their usefulness
and boundaries rather than automatically creating one skill per directory.

### Investigation and deliverables

1. Inventory existing repository instructions, skills, documentation, build/debug
   tooling, and self-test workflows. Confirm what agents can actually run and
   what needs environment setup or permission. Mark unverified capabilities and
   missing prerequisites explicitly.
2. Identify recurring tasks where a skill would reduce context gathering or
   procedural mistakes. For each candidate, record the task, invocation trigger,
   expected inputs, workflow, outputs, completion checks, and relevant sources.
3. Separate genuinely distinct workflows and identify shared references. Decide
   whether each need is best served by a new skill, an extension of an existing
   skill, or documentation alone. Record rationale and avoid duplicating existing
   active or closed tickets.
4. Create one bounded feature ticket per selected skill in `tickets/feature/`.
   Each ticket must follow the ticket guide and include:
   - Agent-assigned priority, status `open`, and required analysis skill
     `mattpocock-skills:grilling`.
   - Concrete use cases, invocation triggers and exclusions, expected inputs,
     workflow steps, outputs, and observable completion criteria.
   - Relevant commands/scripts/symbols, environment prerequisites, architecture
     constraints, error handling, and links to authoritative project references.
   - Proposed skill placement and discovery mechanism, dependencies, scope
     boundaries, and unresolved decisions for its own analysis round.
   - A verification plan using representative agent tasks, checking that commands
     and references work and that the skill leads to useful, verifiable results.
   - A requirement to use the available skill-creation and writing-for-agents
     guidance when authoring the skill, resolving current locations at that time.
   - A link back to this parent ticket and related documentation/tooling tickets.
5. Record the assessed candidates, created-ticket links, recommended execution
   order, and deferred/rejected candidates with concise reasons in this ticket's
   work log. Identify shared documentation/tooling prerequisites explicitly.

This ticket delivers a skill backlog. Skill authoring happens through the child
tickets, each with its own analysis and execution gate. Documentation remains the
reference for implementation details; skills explain when and how to use that
knowledge. Coordinate with the existing documentation and GDB-extension backlogs
so skills describe available behavior and label planned capabilities accurately.

## Source entry points and related work

- [Ticket workflow](../TICKET.MD)
- [Build and QEMU targets](../../Makefile)
- [Debug launcher](../../scripts/debug-kernel.sh)
- [Arx GDB extension](../../scripts/gdb_extention.py)
- [Dynamic userspace fixture build](../../scripts/build-dynamic-fixtures.sh)
- [Self-test infrastructure and suites](../../kernel/selftests)
- [Kernel layers](../../kernel/layers)
- [Platform services](../../kernel/platform)
- [Architecture code](../../arch)
- [Current documentation](../../docs)
- [Project structure documentation ticket](document_project_structure.md)
- [Self-test documentation ticket](document_self_tests.md)
- [GDB-extension investigation and ticket creation](plan_gdb_extension_tickets.md)

Inspect the current instruction/skill locations and available skill catalog during
analysis; their presence, layout, and tooling support must be verified then.

## Acceptance criteria

- [ ] Existing instructions, skills, tooling, and references are inventoried, with actual agent-accessible workflows and prerequisites identified.
- [ ] QEMU/GDB debugging and self-test development are assessed and receive child tickets unless the owner explicitly agrees otherwise.
- [ ] Selected skills address concrete recurring tasks and have distinct triggers, workflow boundaries, outputs, and completion checks.
- [ ] Each selected skill has an individual, prioritized feature ticket with detailed scope, references, authoring guidance, and a representative verification plan.
- [ ] Child feature tickets start `open`, require grilling during analysis, and preserve the owner's execution gate.
- [ ] Skill placement/discovery, shared references, tooling/documentation dependencies, and planned versus existing capabilities are explicit.
- [ ] The work log links all resulting tickets, recommends an execution order, and explains deferred candidates; unfinished assessment or ticket creation is recorded before stopping.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned investigation and backlog creation for Arx agent skills.
QEMU/GDB debugging and working with or adding self-tests are the owner's initial
examples. Analysis and execution have not started.
