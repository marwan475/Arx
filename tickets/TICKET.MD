# Ticket guide

All new code must follow code format in kernel.cpp no compressed lines code follow proper human readable styling

Tickets track work for agents and the project owner. Follow this guide when
creating, analyzing, executing, updating, closing, or reopening a ticket.

## Ticket rules

- Create each ticket as one descriptive `snake_case.md` file in `tickets/feature/`
  or `tickets/debug/`, for example `tickets/debug/fix_usb_keyboard_disconnect.md`.
  When execution starts, move it to `tickets/in_progress/`. Finished work stays
  there until the owner authorizes closure and confirms where to move it next.
  Preserve its `Type` metadata and full work log on each move.
- Choose a name unique across feature, debug, in-progress, and closed tickets. Add a short descriptive
  suffix when needed. Preserve the filename when moving a ticket.
- Feature tickets require `mattpocock-skills:grilling` during analysis. Debug
  tickets require `mattpocock-skills:diagnosing-bugs` during analysis. Record the
  exact required skill in each ticket and follow its instructions.
- Agents assign priority from the table below. Agents may revise it as evidence
  changes; record a brief reason for revisions in the work log.
- Keep the metadata current and append dated work-log entries inside the ticket.
  Preserve previous entries when updating or moving it.

| Priority | Meaning |
| --- | --- |
| P0 | Urgent blocker; address first. |
| P1 | High priority. |
| P2 | Normal priority; default unless the request indicates otherwise. |
| P3 | Low priority. |

| Status | Meaning |
| --- | --- |
| open | Awaiting analysis, including reopened tickets. |
| analyzed | Analysis and plan recorded; awaiting the owner's execution instruction. |
| in progress | Execution started; work, verification, or owner authorization to close remains pending; stored in `tickets/in_progress/`. |
| closed | Acceptance criteria met, required verification passed, and owner authorized closure and the destination; stored where the owner directed. |

Normal lifecycle: `open → analyzed → in progress → closed`.

## Ticket log

Maintain [TICKET_LOG.md](TICKET_LOG.md) whenever creating, analyzing, executing,
updating, moving, closing, or reopening a ticket. Before handing off, make sure
every ticket appears exactly once with its current priority, title, type, status,
and a working link to its current file. Ticket files are the source of truth.

Use the Feature, Debug, In progress, and Closed sections. Feature and Debug list
tickets in their respective directories; In progress lists tickets stored in
`tickets/in_progress/`, including those paused for a revised plan; Closed lists
closed tickets at their owner-approved destinations. Move the existing row when
a ticket changes sections and update its link after moving the file. Show `None.`
when a section is empty.

Sort each section by priority from P0 through P3, then alphabetically by ticket
filename within the same priority. Update the row and ordering when metadata
changes. This current index supplements the dated work logs inside tickets and
the filename-only closure histories in `log.txt`; keep those histories as required
below.

## 1. Creating a ticket

1. Choose the feature or debug directory and a unique filename.
2. Use the template below. Set status to `open`, assign priority, and name the
   required skill for its type.
3. Write a detailed task prompt with enough context for another agent to
   understand the requested work. Define scope and observable acceptance criteria.
   Record missing information explicitly as unknown; keep unresolved criteria
   visible for clarification during analysis.
   - Features: describe the desired behavior, use cases, and scope boundaries.
   - Debugging: describe observed and expected behavior, reproduction steps,
     environment, and available evidence. Mark unavailable details as unknown.
4. Leave the ticket `open`. Creation alone does not start grilling, investigation,
   or implementation. Analysis begins when requested.

## 2. Working on a ticket

Before analyzing, executing, or resuming a ticket, check [`docs/skills/`](../docs/skills/)
for repository skills. Read and follow each applicable skill's `SKILL.md` alongside
the ticket's required skill.

### Analyze first

1. Read the entire ticket and inspect the relevant code and project instructions.
2. Use the ticket's required skill:
   - Features: complete the grilling conversation and obtain the owner's
     confirmation of shared understanding.
   - Debugging: investigate using the diagnosing-bugs skill. Record evidence and
     a diagnosis, or explicitly describe what still requires investigation.
3. Resolve acceptance-criteria questions with the owner. Append a dated
   **Analysis and plan** entry covering affected code, findings, proposed steps,
   verification, risks, and remaining unknowns. If diagnosis is incomplete, the
   plan must identify the next investigation steps.
4. Set status to `analyzed` and stop. Tell the owner the plan is recorded and
   execution is awaiting their instruction. A general initial request such as
   “work on this ticket” does not bypass this pause.

### Execute after instruction

1. Wait for the owner's explicit execution instruction after analysis.
2. Set status to `in progress` when execution begins and move the entire ticket
   to `tickets/in_progress/<ticket_name>.md`, creating the directory if needed.
   Preserve the filename, type, metadata, and history, and check relative links
   after moving. Then carry out the recorded plan and its verification.
3. If scope or the plan materially changes, record an updated plan, set status to
   `analyzed`, and pause for a new execution instruction. Keep an already-started
   ticket in `tickets/in_progress/` during this pause.
4. Before stopping, update the ticket using section 3. The ticket must accurately
   describe the work's state even when execution is interrupted or incomplete.

### Resume unfinished work

Read the ticket and inspect the current code before continuing. Existing
execution authorization remains valid for unfinished work within the recorded
scope and plan. A material change follows the updated-plan pause above.
If a ticket with status `in progress` is still in a feature or debug directory,
move it to `tickets/in_progress/` before resuming.

## 3. Updating, closing, and reopening tickets

### Unfinished or blocked

Keep status `in progress` and store the ticket in `tickets/in_progress/` when
implementation or required verification remains unfinished. Before stopping,
move it there if necessary, preserving its filename, type, metadata, and history.
Append a dated **Progress and remaining work** entry containing:

- What was completed and the current implementation state.
- Verification performed and its results, including failing or unavailable checks.
- Specific remaining tasks, blockers, and the next action needed to continue.

Make the handoff sufficient for another agent to resume without reconstructing
the previous session. Required checks that fail or cannot run keep the ticket
`in progress`; record the exact blocker and next action.

### Work complete; await owner authorization

1. Confirm every acceptance criterion is met and required verification passed.
   Record the relevant checks and results as evidence in the work log.
   For documentation tickets, add or update links to the resulting guides in the
   root README's Documentation section and verify that they resolve before
   requesting closure, unless the owner explicitly directs otherwise.
2. Append a dated **Ready for owner review** entry summarizing the completed work,
   verification, and any deferred TODOs. Keep status `in progress` and leave the
   ticket in `tickets/in_progress/`.
3. Report the result and ask the owner to authorize closure and specify or confirm
   the next destination. Propose `tickets/closed/<ticket_name>.md` as the usual
   destination. Wait for an explicit answer covering both closure and destination;
   ask for whichever remains unspecified. Permission to execute, passing checks,
   and an agent's completion assessment do not authorize closing or moving it.

### Close after owner authorization

1. Once the owner explicitly authorizes closure and the next destination, append
   a dated **Completion** entry recording that authorization and a concise summary
   of what changed (normally one to three sentences).
2. Set status to `closed` and move the entire file to the owner-approved
   destination, preserving its filename, metadata, type, and history. Check
   relative links after moving.
3. Append the ticket filename, including `.md`, to both `tickets/closed/log.txt`
   and the originating directory's log (`tickets/feature/log.txt` or
   `tickets/debug/log.txt`, according to the ticket's type). Use one filename per
   line, with no dates, summaries, or other text. Preserve existing entries and
   create either log if it does not exist.

### Reopen

If completed work proves incomplete or incorrect, append a dated **Reopened**
entry explaining why, set status to `open`, and move the file back to
`tickets/feature/` or `tickets/debug/` according to its type. Repeat analysis and
the owner's execution gate before resuming implementation.

## Ticket template

Replace placeholders when creating the ticket. Work-log entries are added as the
corresponding events occur; use dates in `YYYY-MM-DD` format.

```markdown
# <Descriptive title>

- Type: <feature | debug>
- Priority: <P0 | P1 | P2 | P3>
- Status: open
- Required skill: <mattpocock-skills:grilling | mattpocock-skills:diagnosing-bugs>

## Task prompt

<Detailed request, context, desired outcome, and scope boundaries.>
<For bugs: observed versus expected behavior, reproduction, environment, evidence.>
<Explicitly identify unknown information.>

## Acceptance criteria

- [ ] <Observable outcome required for completion.>

## Work log

### YYYY-MM-DD — Created

<Brief context and reason for the assigned priority.>
```
