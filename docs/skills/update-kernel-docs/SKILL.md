---
name: update-kernel-docs
description: When explicitly invoked by the owner, keep existing Arx documentation current after implementation changes or refresh a guide. Make only important, concise updates in the existing structure and style. Use write-kernel-docs for a new guide.
---

# Update existing kernel documentation

Use this skill only when explicitly invoked by the owner.

After implementation and required checks are finished, review the affected docs
before the final handoff. Document the resulting feature or behavior where readers
already expect to find it. The owner's current instructions take precedence.

## Decide whether an update matters

Read the final implementation, relevant tests, and the existing guide for the
changed area. Check its behavior descriptions, component tables, limitations,
source filenames, and diagrams.

Update documentation when a reader needs to know about a changed capability,
contract, failure behavior, initialization dependency, ownership rule, execution
constraint, support status, or important usage instruction. Fix descriptions and
references that the change makes inaccurate.

An internal refactor or a fix restoring already-documented behavior may need no
prose change. Review documentation impact for every implementation change; edit
only when there is meaningful information to add or correct. Avoid inventing a
documentation addition just to record that work happened.

## Make the smallest useful edit

- Replace or extend the relevant sentence, table row, or subsection. Remove
  obsolete claims as part of the same edit, including limits that no longer apply.
- Follow the guide's existing headings, initialization/runtime order, terminology,
  detail level, and human-readable voice. Add a short subsection only when the
  feature does not fit an existing one.
- Describe what the kernel now does, how the feature is used, and essential
  constraints. Include only the structure or mechanism needed to understand it.
- Keep source references as plain filenames, matching the existing guides. Keep
  layer inventories to one-line responsibilities and deeper details in their
  owning component guide.
- Write current-state documentation, not a large summary of the implementation
  work. Leave dates, changed-file lists, session history, verification reports,
  and “what I did” narratives out of the feature documentation.

For example, replace an obsolete limitation with “Uses firmware NUMA topology,
with a one-node fallback,” rather than appending a report about adding NUMA support.

Keep related guides consistent only where the change affects their claims. Update
README navigation if a guide moves or a new guide is introduced; keep design
explanations in the docs rather than repeating them in README.

## Check diagrams and scope

Update an existing diagram only when the changed behavior makes it inaccurate or
omits an important part of the documented flow. Keep its editable Mermaid source
and embedded SVG consistent. For rendering and visual checks, follow the diagram
instructions in [write-kernel-docs](../write-kernel-docs/SKILL.md). Preserve unrelated
diagrams, formatting, and sections.

Reuse established documentation choices for targeted edits; routine maintenance
does not require another grilling round. If a genuinely new guide or substantial
restructure is needed, use write-kernel-docs to agree that broader scope.

When working through a ticket, follow [TICKET.MD](../../../tickets/TICKET.MD) for
work-log updates and approval gates. Keep implementation summaries in the ticket
or final response, separate from the human-facing guide. Direct edits require no
new ticket unless requested.

## Verify and hand off

Check the revised claims against the completed implementation, resolve changed
filenames and links, and inspect the diff for unnecessary expansion or duplication.
If a diagram changed, render and visually inspect its SVG. Documentation-only
edits normally need these checks, not another kernel build or boot.

Finish the relevant documentation updates before reporting the implementation
complete. Briefly name the docs updated, or note that the impact review found no
important documentation change when relevant; do not turn the guide into a work log.
