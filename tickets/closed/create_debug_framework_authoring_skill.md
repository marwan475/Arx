# Create an agent skill for adding debug statistics and rich views

- Type: feature
- Priority: P2
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Create a repository agent skill for adding useful tracked fields and rich views
to the new debug framework during an authorized investigation, or proposing a
follow-up improvement after an issue is understood. Agents should reuse the
DebugManager contract, manager-owned headers and common GDB helpers instead of
adding another standalone command or repeating broad source/context gathering.

Follow [the workflow](../TICKET.MD) and complete this ticket's own grilling and
execution gate. Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Coordinate with [agent-skill planning](../feature/plan_kernel_agent_skill_tickets.md), whose
QEMU/GDB usage skill is distinct from authoring providers/views. Dependencies:
[foundations](../closed/build_debug_framework_foundation.md), at least one verified stats
and rich-view provider, and the implemented portions of the
[command/maintainer guide](../in_progress/migrate_debug_commands_and_document_framework.md).
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

- [x] A discoverable repository skill has distinct stats/view-authoring and post-investigation opportunity triggers, expected inputs, exclusions and outputs.
- [x] The workflow reuses the real framework and guide and distinguishes generic fields, maintained stats, rich collections and absent/history-only data.
- [x] Debug/release, registration/lifetime, bounded/no-inferior-call inspection and future-native-reader requirements are actionable and source-backed.
- [x] The skill follows existing authorization/ticket rules and produces a follow-up ticket when implementation is outside scope rather than granting itself permission.
- [x] Representative simple-stat, rich-collection and after-fix scenarios have been walked through; references/commands and discovery instructions work.
- [x] Skill-creator and writing-for-agents guidance was applied; no duplicated generic diagnosis/setup guide, Request provider or dump implementation is introduced.

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

## Verification scope

Keep verification proportional to this ticket's changes. Select the smallest set
of checks that demonstrates the acceptance criteria and covers the affected
behavior. Run the relevant checks once after the final changes; stop when they
pass. Repeat or broaden checks only after a new change, a failure, or a concrete
unresolved risk, and record the reason. Do not add repeated QEMU boots, unrelated
suite runs, or baseline investigations merely for extra confidence. Record
incidental failures and their impact; investigate further only when evidence
makes them a blocker for this ticket. Preserve required acceptance checks and
report any checks that could not run.

## Work log

### 2026-10-18 — Created

P2: captures the owner's authoring workflow once the shared framework and examples
exist. Distinct from the broader QEMU/GDB usage-skill backlog; no skill authored yet.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

### 2026-10-20 — Owner-directed verification scope

The owner requested avoiding excessive verification on remaining tickets. Added
an explicit stopping rule and evidence-based limits on repeated or broader
checks. Acceptance criteria, implementation scope, status, and execution gates
are unchanged.

### 2026-10-20 — Analysis started

Owner requested the agent authoring skill. Read the full ticket, skill-creator,
writing-for-agents including SKILL-MECHANICS.md, required grilling and applicable
repository code/documentation guidance. Inspecting existing framework examples
and repository discovery before agreeing the plan. No skill has been authored.

The proposed docs/skills/add-debug-view/SKILL.md placement is writable and matches
existing repository skills. TICKET.MD already directs ticket agents to inspect
docs/skills/ before analysis/execution; this is repository discovery, not global
installation. README and the common maintainer guide can link the skill for direct
discovery outside tickets. There is no repository AGENTS.md in the current file
inventory. Do not add global installation or write .agents/.codex as a workaround.

Proposed packaging is a focused SKILL.md linking the existing framework guide,
ticket workflow and relevant examples, without a ticket generator or duplicated
setup manual. Skill-creator's quick_validate.py is available for packaging checks.
Placement, discovery and scope confirmation remain pending in the grilling round.

### 2026-10-20 — Analysis and plan; execution authorized

The owner answered "yes and go", confirming the proposed scope and authorizing
execution after the grilling round. Use docs/skills/add-debug-view/SKILL.md with
name add-debug-view, discovered through the existing TICKET.MD docs/skills scan
and new README/common-guide links. Keep normal model-facing trigger wording for
authorized authoring and post-investigation opportunities. This remains a
repository-local skill, without global installation or .agents/.codex edits.

Inspected actual CredentialDebugStats/CredentialManager updates and Dispatcher
publication, IpcDebugStats/IpcState/Communication.cpp/communication.py and host
fixtures, controlled foundation adapters and the current command/maintainer
guide. Generic stats additions need no new adapter; rich collections have typed
roots/versions, borrowed lifetime, shared budgets and native semantics.

Plan: write one concise SKILL.md linking the existing guide, ticket rules and
repository code/documentation skills. Route ordinary fields, counters, bounded
collections, documentation corrections and absent/history-only evidence
separately. Preserve authorization, release erasure, composition publication,
partial results and no-target-call contracts. Provide focused existing examples
rather than a second setup manual, generator, script or UI metadata package.

Validate frontmatter/packaging and all links, then walk through a simple counter,
a bounded production collection with invalid-state handling and an after-fix
opportunity outside scope. Use source-backed reviewed walkthroughs only; no
production kernel edits or invented follow-up ticket are needed for this skill
test. Update README and the common guide's authoring pointer, inspect the diff
for duplicated instructions, and record outcomes. Kernel builds/boots are not
needed for this instruction-only artifact. Bundled quick_validate.py exists but
the selected Python currently lacks PyYAML; attempt it and record any fallback
packaging checks accurately without installing dependencies blindly.

Moved to tickets/in_progress/ before authoring. No remaining owner design choice
requires another pause; closure still follows TICKET.MD.

### 2026-10-20 — Ready for owner review

Created docs/skills/add-debug-view/SKILL.md as the agreed one-file repository
skill, with separate authorized authoring and post-investigation opportunity
triggers, expected context, representation choices, counter semantics,
release-erasing instrumentation, composition publication/borrowed lifetimes,
bounded semantic adapters and future-native-reader contracts. It reuses the
common guide, ticket rules and add-arx-code/update-kernel-docs rather than
duplicating build/attach training or installing global configuration. Added
README Documentation and common maintainer-guide pointers; updated all moved
ticket links. No kernel, framework implementation, helper script, UI metadata or
actual follow-up issue was added to exercise the skill.

Applied skill-creator and writing-for-agents including SKILL-MECHANICS.md:
discriminating frontmatter, conditional reference loading, checkable completion
criteria and a single source of truth for detailed framework mechanics. The
existing TICKET.MD docs/skills scan provides ticket discovery; the new README and
guide pointers provide direct discovery without claiming global registration.

An independent read-only forward test followed the finished skill against:

1. Existing combined filesystem-ID refusals: correctly selected
   `arx-debug credentials stats` / identities.refusals, traced ChangeIdentity's
   SET_FS_ID decision and DebugOperation::Finish(3), and identified the existing
   CredentialManager debug_outcomes fixture. It proposed no unnecessary code.
   The explicitly authorized UID/GID-split variant identified two stats fields,
   the existing debug-only decision block, the group argument and guarded atomic
   updates. Existing Dispatcher sizeof-based publication suffices; no new adapter
   or parser selector is required. Its plan covered refusal/allowed/no-op/error
   semantics, affected credential fixtures and debug/release evidence.
2. Wrapped pipe packets: correctly reused ipc pipes and traced IpcState.hpp,
   runtime ring mutations, IpcDebugStats.hpp, Communication.cpp, communication.py
   and existing kernel subsystem/integration fixtures. It checked physical
   capacity/head/size and ring arithmetic, bounded metadata reads without default
   payload copying, identity checks, corrupt/stale links and partial results.
   Fixed local packet-bit processing is physically capped; this is not a promise
   to render packet output through every corrupt weak-link case.
3. Absent past wakeup order after an authorized fix: identified current scheduler
   fields/totals as insufficient historical evidence, checked the ticket index,
   and drafted only a P2/open follow-up with a provisional interface, source
   transitions, ordering/loss/identity/storage unknowns and acceptance checks.
   It neither implemented history nor reopened the original work. The proposed
   ticket was a validation artifact in the response, not a new repository issue.

The forward test found one stale example link to debug/tests/test_communication.py;
that directory is absent in the current checkout. Corrected the skill to link
the existing kernel IPC selftest and distinguish subsystem fixtures from adapter
implementation. Removed tests were not recreated or represented as runnable.

Verification completed:

- Bundled skill-creator/scripts/quick_validate.py: `Skill is valid!`.
  Initial invocation found missing PyYAML. After explicit approval, installed
  PyYAML 6.0.3 into an isolated temporary directory under the workspace, ran the
  unmodified validator with that temporary PYTHONPATH, and removed the directory.
  The global Python environment and project dependencies were not changed.
- All 201 local Markdown targets in the changed files resolved, including skill,
  README/guide discovery, source examples and current ticket links. Verified the
  skill folder contains only SKILL.md, the name matches its directory, content
  remains under 1,500 words and no temporary validation directory remains.
- Reviewed instruction claims against the source and forward-test findings;
  whitespace/diff checks passed. No kernel build or boot was required for this
  instruction-only change, and none is claimed.

All acceptance criteria are met, with no implementation TODOs. Status remains
in progress pending owner authorization to close and move to
tickets/closed/create_debug_framework_authoring_skill.md under TICKET.MD.

### 2026-10-21 — Completion

The owner instructed "close" in response to the proposed tickets/closed/
destination, authorizing closure and the move. Created the add-debug-view
repository skill, README/maintainer-guide discovery links and source-backed
examples; packaging validation, independent scenario walkthroughs, local links
and whitespace checks passed. No kernel changes or global skill installation
were introduced.
