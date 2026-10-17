---
name: write-kernel-docs
description: Create Arx kernel architecture, layer, manager, and subsystem guides, or agree substantial documentation restructuring, using the project's human-readable format and source-verified diagrams.
---

# Write Arx kernel documentation

Write for humans learning the implementation. Explain responsibilities, boundaries,
and actual behavior in concise prose. The owner's current instructions override
these project defaults.

For concise maintenance of an existing guide after code changes, the owner can explicitly invoke
[update-kernel-docs](../update-kernel-docs/SKILL.md).

## Agree the scope

For a new guide, establish its audience, filename, directory, depth, and diagrams
before writing. Use the available grilling skill when requested or required by a
ticket. Ask numbered rounds with a recommended answer for each independent
question, worded so yes accepts the recommendation. Wait for actual answers after
each round; end the turn rather than continuing investigation, delegation, or
writing while questions are pending. Settle dependent choices in later rounds and
obtain confirmation of the agreed scope before implementation.

Reuse choices already settled in the conversation. A targeted revision to an
existing guide normally needs no new interview. Use the existing correctly spelled
directories and agree new filenames; avoid creating competing directory spellings.

For ticket work, read and follow [TICKET.MD](../../../tickets/TICKET.MD) for analysis,
execution authorization, status, moves, logs, review, and closure. It is the source
of truth for that lifecycle. Direct documentation requests do not require creating
a ticket or adding ticket approval gates. Respect an explicit request to skip one.

## Research the implementation

Read the relevant interfaces, implementation, callers, assembly where applicable,
and existing tests. Establish initialization order and runtime paths from actual
calls. Record research evidence in the ticket when one exists.

Separate intended design from verified behavior, partial support, stubs, and
unknowns. Read stub bodies: a success-returning no-op is not implemented hardware
support. Keep architecture-specific guides separate and preserve unsupported code
unless implementation work was requested. Record implementation concerns separately
from documentation changes.

Use the other guides for orientation, then verify claims against source. Examples
of the established format are [KernelArch.md](../../architecture/KernelArch.md),
[Platform.md](../../architecture/platform/Platform.md), and
[x86_64.md](../../architecture/arch/x86_64.md). Read only the examples relevant to
the current guide.

## Write the guide

Use a descriptive title, a short explanation of the component's role, and clear
sections. Follow initialization order first, then runtime behavior. Keep startup,
request handling, and return/cleanup flows distinct when combining them would be
confusing.

Cover the details that matter at the agreed depth:

- What the area owns, who calls it, and which services it uses.
- What each initialization stage establishes and what must exist first.
- Key structures, data layouts, calling conventions, and ownership/lifetimes.
- Relevant locking, interrupt, preemption, and execution-context constraints.
- Implemented paths, failure behavior, limitations, and useful test/debugging files.

Use compact tables for component inventories, structures, support status, or
comparisons. List each layer's managers and principal components with one-line
responsibilities when documenting a layer; obtain the inventory from its factory.
Keep deeper manager internals in the corresponding focused guide.

Use plain filenames such as `Dispatcher.cpp` in the human-facing prose: no relative
source paths or clickable source links. Functions and types may use inline code.
Explain repository-specific mechanics without generic kernel tutorials or agent
handoff language. Select detail that helps a reader understand a responsibility or
flow; avoid exhaustive syscall or symbol catalogs unless requested.

## Diagrams and navigation

When diagrams are agreed, keep editable Mermaid sources in `docs/DIAGRAMS.md`
and rendered SVGs in `docs/imgs/`. Embed the SVG files in the guide using image
paths relative to that guide; use SVGs rather than inline Mermaid or rasterized
screenshots. Existing diagram work is a source to verify and reuse, not proof of
current behavior.
If diagrams are deferred, record an explicit TODO in the ticket or agreed follow-up
and distinguish it from the current completion criteria.

Use the existing `platform-mermaid-config.json` style unless another style was
agreed. Render with Mermaid CLI or another available Mermaid renderer. Preserve
native SVG text, including text spacing (`xml:space="preserve"` on the SVG root),
and provide useful titles and descriptions. Check actor boundaries, branch labels,
initialization order, and implemented versus unavailable paths against code.

Preserve useful existing images. Correct stale labels or simplified flows when
updating their sources; explain retained simplifications briefly in captions.
Move existing embeds and accompanying explanations when the owner requests a
migration, updating their relative paths. Leave unrelated source blocks intact.

Add or update links to finished guides in the root README's Documentation section,
unless the owner directs otherwise. Keep detailed design explanations in the
guides. Remove duplicated README material within the agreed scope; retain unique
material or move it to an appropriate guide before removing it. Preserve the
README's original architecture image and third-party/license information unless
the owner requests changing them.

## Verify and deliver

Check every cited filename, local document link, and image path. Compare inventories
with factories and layout claims with headers, static assertions, and assembly.
Confirm support tables match function bodies and that diagrams match the prose.

Parse SVGs and render previews for visual inspection: check readable text, spacing,
clipping, and arrows. Verify the guide embeds SVGs, and preserve the Mermaid sources
and rendering configuration needed to update them. Run whitespace/diff checks and
confirm that documentation-only work left implementation files unchanged.

Prose-only changes normally need source, link, inventory, and visual checks rather
than a kernel build or boot. Report which verification actually ran; a source
inventory is not a successful runtime test. When ticket verification is complete,
follow TICKET.MD's owner-review and closure gate. For direct work, deliver the
verified documents and summarize any remaining limitations.
