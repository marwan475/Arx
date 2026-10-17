# Investigate Arx GDB extension opportunities and create implementation tickets

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Inspect Arx's existing GDB extension and kernel implementation to identify useful
debugging commands, views, and improvements. Turn the findings into concrete,
prioritized feature tickets for later implementation in
`scripts/gdb_extention.py`. The owner is an experienced kernel developer; focus
on Arx-specific investigation needs and reducing the context agents must gather
to diagnose failures.

Follow [the ticket workflow](../TICKET.MD). During analysis use
`mattpocock-skills:grilling` to establish the owner's debugging pain points,
preferred command behavior, target architectures, and investigation priorities.
Record an analysis and plan, set this ticket to `analyzed`, and wait for the
owner's explicit execution instruction before carrying out the ticket-creation
work. Detailed inventory, selection, and child-ticket creation are execution
deliverables; initial inspection supports the analysis.

### Investigation and deliverables

1. Inventory existing commands, their arguments, data sources, pointer discovery,
   output, and current limitations. At creation time the script registers
   `arx-platform`, `arx-pmm`, `arx-vmm`, `arx-cpus`, `arx-pci`, `arx-heap`,
   `arx-vfs`, `arx-initramfs`, `arx-proc`, `arx-task`, and `arx-rsrc`.
   Recheck this inventory against the current script when executing.
2. Compare existing coverage with real subsystem state and likely failure paths.
   Explore scheduling/run queues and blocked tasks, process lifecycle, address
   spaces/page faults/TLBs, locks and wait queues, signals, events, sockets/IPC,
   descriptors/VFS, DMA, interrupt/syscall state, and self-test progress/results.
   These are investigation candidates, not predetermined commands or quotas.
3. For each worthwhile opportunity, identify a concrete debugging question,
   existing manual investigation steps, source-backed data that a command could
   expose, and how it would improve diagnosis. Distinguish new commands from
   improvements to existing commands and shared infrastructure.
4. Consider feasibility across boot phases and architectures, symbol/type
   availability, pointer resolution, stopped versus running targets, partial or
   corrupt state, traversal bounds, and the cost of reading target memory.
   Favor read-only inspection; any proposed state-changing behavior requires an
   explicit rationale and agreement during the child ticket's analysis.
5. Create one reasonably bounded feature ticket per selected addition or
   improvement in `tickets/feature/`, following the ticket guide. Each needs:
   - A descriptive filename, agent-assigned priority, status `open`, and required
     skill `mattpocock-skills:grilling`.
   - The failure scenario or investigation question, intended command interface
     and example output, relevant source symbols/structures, proposed integration
     points, constraints, dependencies, unknowns, and scope boundaries.
   - Observable acceptance criteria and a verification approach using relevant
     kernel/self-test state or controlled fixtures, including unavailable or
     invalid target state where applicable.
   - A link back to this parent ticket and related documentation tickets where
     they provide useful context. Child tickets retain their own analysis and
     execution gate; this investigation does not authorize their implementation.
6. Record a concise inventory and gap assessment in this ticket's work log, with
   links to the created tickets and a suggested implementation order. Record
   rejected or deferred candidates with brief reasons to avoid repeated research.
   Check existing active and closed tickets to avoid duplicate proposals.

This ticket produces a debugging-extension backlog. Keep implementation changes
in the resulting tickets. Record confirmed defects in current tooling as debug
tickets requiring `mattpocock-skills:diagnosing-bugs`, and distinguish them from
feature opportunities. Agree the final number and grouping of tickets during
analysis rather than inventing work to meet a count.

## Source entry points

- [GDB extension](../../scripts/gdb_extention.py)
- [Debug launcher](../../scripts/debug-kernel.sh)
- [Build and QEMU targets](../../Makefile)
- [Architecture contracts and implementations](../../arch)
- [Platform services](../../kernel/platform)
- [Layer managers and Dispatcher](../../kernel/layers)
- [Self-test infrastructure and suites](../../kernel/selftests)
- [Project structure documentation ticket](../closed/document_project_structure.md)
- [Self-test documentation ticket](document_self_tests.md)

## Acceptance criteria

- [ ] Existing commands and meaningful coverage gaps are inventoried against the current implementation.
- [ ] Selected proposals address concrete debugging scenarios and identify inspectable kernel state and extension integration points.
- [ ] Feasibility, architecture/boot-phase constraints, robustness, dependencies, and unknowns are recorded for each proposal.
- [ ] Selected additions and improvements have individual, actionable feature tickets with priorities, detailed prompts, acceptance criteria, and verification approaches.
- [ ] Child feature tickets start `open`, require grilling during analysis, and link back to this ticket; duplicate tickets are avoided.
- [ ] The work log links all created tickets, recommends an implementation order, and explains deferred candidates.
- [ ] Completion closes this planning ticket with a concise summary; any unfinished investigation or ticket creation is explicitly recorded before stopping.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned investigation and backlog creation for Arx-specific GDB
debugging extensions. Command inventory was checked to ground this prompt;
analysis and execution have not started.
