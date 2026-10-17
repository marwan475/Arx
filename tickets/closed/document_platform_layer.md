# Document platform services

- Type: feature
- Priority: P2
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Create a concise, human-readable guide in `docs/architecture/platform/Platform.md`,
using the same format as `KernelArch.md`: initialization order, plain filenames,
and no clickable source links. Move the platform and memory explanations and ten
diagram embeds from README into the new guide. Keep image files in `docs/imgs/`,
remove the migrated README sections, and add no README navigation link. Preserve
the existing diagrams and explain outdated labels or simplified flows in captions.

Document platform services at orientation depth. Map CPU/SMP, memory (PMM, VMM, heap, metadata, NUMA), DMA, devices, ACPI and terminal services. Explain bootstrap dependencies, arch hooks, contracts consumed by Resource managers, and the role of klib. Distinguish platform mechanisms from higher-level ownership/policy; explain integration boundaries with vendored uACPI/flanterm without documenting their entire upstream internals.

Keep this guide focused on responsibilities and interactions; leave detailed
manager internals and architecture mechanics to their component guides.

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

Coordinate structure with [the kernel architecture overview](document_project_structure.md). Formal layer guides own factory/capability explanations; component tickets own component internals.

## Source entry points

- [kernel/platform/platform.h](../../kernel/platform/platform.h)
- [kernel/platform](../../kernel/platform)
- [kernel/bootstrap.c](../../kernel/bootstrap.c)
- [klib](../../klib)
- [kernel/selftests/platform](../../kernel/selftests/platform)

These are starting points; inspect callers, dependencies, and tests before making claims.

## Acceptance criteria

- [x] `Platform.md` is concise, human-readable, and follows initialization order.
- [x] Platform state, terminal, CPU/SMP, memory/NUMA/metadata, ACPI, architecture setup, devices, DMA, and self-tests have brief explanations of responsibilities and users.
- [x] Existing platform and memory explanations and all ten diagram embeds are migrated out of README; image files remain unchanged in `docs/imgs/` and README gains no navigation link.
- [x] Important dependencies, resource lifetimes, locking constraints, and one allocation flow are explained briefly; outdated claims and diagram labels are corrected in prose.
- [x] Source filenames are plain text with no clickable source links; embedded image paths resolve, and relevant limitations and test filenames are identified.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned documentation work in the codebase understanding backlog.

### 2026-10-17 — Analysis and plan

Completed the required grilling conversation. The owner confirmed the path,
format, migration scope, and concise service coverage, then explicitly instructed
“yes go” to execute that agreed plan. This entry supersedes the earlier broad
requirements for deep contracts, debugging-tool coverage, and clickable links.

Plan: write the guide in initialization order from `bootstrap.c`; migrate the
README block from Platform Setup through Post Platform Init; embed the ten existing
platform/memory SVGs at their relevant explanations without moving image files.
Keep the overall architecture image and the rest of README. Explain platform
versus Dispatcher naming and NUMA/zone simplifications in existing diagrams.
Cover service users, basic locking/lifetime rules, a short allocation flow, and
platform tests without duplicating manager internals.

Source findings: PMM now reads and validates NUMA topology and falls back to one
node, contrary to README's one-node-only claim. It has Normal and DMA zones and
tries other nodes when local memory is exhausted. The diagram's single node/zone
is an overview. Bootstrap initializes terminal, CPUs, PMM, VMM, heap, ACPI,
architecture, devices, x86 DMA, and other CPUs before platform tests and the stack
handoff. Existing diagrams also use Dispatcher to mean platform state. Preserve
the drawings and clarify these differences in captions.

Verification: compare initialization and service claims with implementations,
verify all ten embeds against original README and image hashes, check filenames
and image paths, and verify README outside the migrated block is unchanged.
No runtime tests are required for prose/image-embed relocation. No unresolved
scope decisions remain; diagram details may need concise caveats, not code fixes.

### 2026-10-17 — Execution started

Proceeding on the owner's explicit “yes go” instruction after scope confirmation.
Moved the ticket to `tickets/in_progress/`. Leave it there awaiting owner review,
closure authorization, and destination confirmation after verification.

### 2026-10-17 — Ready for owner review

Created `docs/architecture/platform/Platform.md` in the agreed concise,
human-readable format, with ten sections following platform initialization.
Migrated the ten existing platform/memory SVG embeds and the relevant explanations
from README; retained all image files unchanged. Corrected stale bootstrap names
and NUMA assumptions in the migrated prose, clarified diagram labels and heap-list
naming, and covered platform service users, allocation flow, limits, and tests.

Verification passed:

- All ten embeds match the original README diagram filenames and resolve from
  the new document; SVG contents were parsed during research.
- Image hashes and `KernelArch.md` match their pre-edit snapshots.
- README is byte-for-byte unchanged outside the migrated Platform Setup through
  Post Platform Init block; no README navigation link was added.
- All cited source filenames exist, no source paths or clickable text links are
  present in the guide, and whitespace checks pass.
- Initialization, NUMA/zone fallback, allocator behavior, metadata synchronization,
  ACPI scope, DMA limitations, and test order were checked against the relevant
  implementations. No build/runtime tests were needed for documentation changes.

All current acceptance criteria are met. Status remains `in progress` in
`tickets/in_progress/`, awaiting owner review, explicit authorization to close,
and confirmation of the next destination. No implementation changes were made.

### 2026-10-17 — Completion

The owner explicitly authorized closing this ticket, moving it to
`tickets/closed/`, and updating both logs. Completed the platform guide and README
diagram migration; the verification recorded above passed. Appended this filename
to the feature and closed logs.
