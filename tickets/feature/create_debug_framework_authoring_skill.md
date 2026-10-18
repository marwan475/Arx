# Create an agent skill for adding debug statistics and rich views

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Create a repository agent skill for adding useful tracked fields and rich views
to the new debug framework during an authorized investigation, or proposing a
follow-up improvement after an issue is understood. Agents should reuse the
DebugManager contract, manager-owned headers and common GDB helpers instead of
adding another standalone command or repeating broad source/context gathering.

Follow [the workflow](../TICKET.MD) and complete this ticket's own grilling and
execution gate. Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Coordinate with [agent-skill planning](plan_kernel_agent_skill_tickets.md), whose
QEMU/GDB usage skill is distinct from authoring providers/views. Dependencies:
[foundations](../closed/build_debug_framework_foundation.md), at least one verified stats
and rich-view provider, and the implemented portions of the
[command/maintainer guide](migrate_debug_commands_and_document_framework.md).
See [assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment).

### Proposed placement and discovery

Use `docs/skills/add-debug-view/SKILL.md` with supporting references/scripts only
where they remove repeated work. Confirm the final skill name, trigger and how
repository agents discover it during analysis; inspect the then-current skill
catalog and project instruction mechanism. Do not assume placing a file in
`docs/skills/` automatically installs it globally. Do not write to read-only
`.agents`/`.codex` paths or global skill locations as a discovery workaround.

When authoring, use the available `skill-creator` and
`mattpocock-skills:writing-for-agents` guidance, resolving their current locations
and reading their SKILL.md files then. Use the applicable code/documentation
skills for supporting implementation and references. This ticket creates a real
skill, not a duplicate architecture or command reference.

### Invocation, inputs and exclusions

- Trigger: an agent needs missing debug information for a concrete issue; a user
  asks to extend/debug the framework; or an investigation identifies a reusable
  future view. Inputs are the debugging question, affected subsystem/source state,
  current authorization/ticket, matching debug environment and available evidence.
- Exclude generic debugging diagnosis/build/attach training, ordinary kernel
  feature work without an information gap, Request-layer providers, production
  telemetry, historical tracing and dump mechanics. Link the broader debugging
  skill/reference for setup instead of copying it.
- The skill never grants implementation permission. Within existing authorized
  scope, follow that scope and ticket workflow. If the opportunity is follow-up
  work, create/propose an open feature ticket with enough context rather than
  implementing it after the original issue or automatically reopening closed work.

### Required workflow

1. State the investigation question and check existing generic stats/views first.
   Decide whether it needs an ordinary field, maintained counter, collection root,
   rich adapter or only a documentation correction. Do not make one provider per
   Request wrapper or one command per manager.
2. Inspect the real source/type/update sites, ownership/lifetime, C versus C++
   storage, boot availability, concurrency and bounds. Distinguish current stored
   state from derived interpretation and new historical retention. Document units
   and successful/attempted operation meanings; do not invent data that is absent.
3. Add manager-owned debug headers/updates and production registration through the
   framework, with no work/storage/argument effects in release. Use shared update,
   type/collection and status helpers; avoid direct upward dependencies.
4. Prefer generic rendering for stats. For rich views, reuse bounded readers and
   return useful data/status independently of formatting. Preserve typed roots,
   bounds, identities and source semantics for a future native consumer. Do not
   pretend a Python-only callback can run in a kernel dump path.
5. Verify known useful state, invalid/unavailable/partial/truncated data,
   debug/release exclusion and relevant subsystem behavior. Never call target
   getters, acquire stopped-target locks, mutate the inferior or install tools
   blindly. Record environment prerequisites and verification actually performed.
6. Update the common command/maintainer guide and relevant architecture claims,
   then record result/evidence/remaining work in the authorized ticket. A follow-up
   opportunity should include a concrete question, sources, interface example,
   constraints, dependencies and acceptance/verification criteria.

### Representative outputs

```text
Need: distinguish free-page totals from available contiguous DMA blocks.
Decision: add per-node/per-zone/order stats and a bounded block view.
Evidence: known allocation/free fixture; corrupt-list fixture; DEBUG=0 removal.
Docs: command example, units, bounds and partial-update meaning updated.
```

An after-fix result may instead be an open feature ticket. It must clearly say
implementation is pending and identify what was actually observed versus proposed.

### Sources and open questions

- [existing repository skills](../../docs/skills), [ticket rules](../TICKET.MD),
  [Makefile](../../Makefile), [source assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment).
- Actual `debug/kernel/`, `debug/gdb/` and `docs/debugging/GdbCommands.md` must exist
  and be verified when the skill is authored; planned paths are not current facts.
- Resolve discovery mechanism, concise trigger/exclusion wording, whether a
  follow-up ticket helper is worthwhile, and representative provider examples
  during analysis. Reuse a verified view with collections as well as simple stats.

## Acceptance criteria

- [ ] A discoverable repository skill has distinct stats/view-authoring and post-investigation opportunity triggers, expected inputs, exclusions and outputs.
- [ ] The workflow reuses the real framework and guide and distinguishes generic fields, maintained stats, rich collections and absent/history-only data.
- [ ] Debug/release, registration/lifetime, bounded/no-inferior-call inspection and future-native-reader requirements are actionable and source-backed.
- [ ] The skill follows existing authorization/ticket rules and produces a follow-up ticket when implementation is outside scope rather than granting itself permission.
- [ ] Representative simple-stat, rich-collection and after-fix scenarios have been walked through; references/commands and discovery instructions work.
- [ ] Skill-creator and writing-for-agents guidance was applied; no duplicated generic diagnosis/setup guide, Request provider or dump implementation is introduced.

## Verification approach

Use a controlled representative authoring task such as adding a counter to an
existing debug record, then a bounded collection view with invalid-state handling.
Check that the instructions identify correct source/update/registration sites,
reuse existing helpers and require meaningful debug/release and GDB evidence.
Walk an after-fix opportunity outside authorization and verify it ends in a scoped
open ticket, not an implementation. These checks may be reviewed walk-throughs
or permitted scratch fixtures; do not make unrelated production changes just to
test the skill. Validate skill packaging with the then-available skill guidance,
all local links and repository discovery, and record remaining environment limits.

## Work log

### 2026-10-18 — Created

P2: captures the owner's authoring workflow once the shared framework and examples
exist. Distinct from the broader QEMU/GDB usage-skill backlog; no skill authored yet.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
