---
name: add-debug-view
description: Add Arx debug statistics or bounded rich views for a concrete investigation, or turn a post-investigation information gap outside the authorized scope into an open follow-up ticket. Use for DebugManager provider/view authoring, rather than generic diagnosis or debugger setup.
---

# Add debug statistics and views

This is a repository skill. Ticket agents discover it through the
[ticket workflow](../../../tickets/TICKET.MD); direct requests can name this file.
Its presence here does not install it globally.

Use the [command and maintainer guide](../../debugging/GdbCommands.md) as the
framework reference. Read **Kernel record interface** for stats/publication and
**Rich views and future native readers** when a collection needs an adapter.
Build, attach and ordinary diagnosis belong to that guide and the investigation's
debugging workflow. This skill covers stored debug evidence, excluding Request
providers, production telemetry, historical tracing and dump implementation.

## Establish the information gap and scope

Identify the concrete debugging question, affected source/state, current
authorization or ticket, available evidence and matching debug environment.
Check existing provider stats, rich views and typed `inspect` before adding work.
Distinguish an unread field from information the kernel never recorded.

Choose the smallest change that answers the question:

| Need | Route |
| --- | --- |
| Existing stored field | Use generic stats/typed inspection; correct documentation if discovery was the problem. |
| Count or current gauge across runtime operations | Extend the owning debug record at the actual update sites. |
| Collection order, identity or relationships | Publish a bounded typed root and add a semantic adapter only where generic rendering is insufficient. |
| Evidence absent from current state, especially history | State the limitation; propose a separately scoped capability instead of inferring events that were never retained. |

Follow existing authorization and the ticket lifecycle without asking again for
work already covered. If an after-fix opportunity lies outside that scope, check
the [ticket index](../../../tickets/TICKET_LOG.md) and record an open feature
ticket under TICKET.MD, or add evidence to an existing open ticket. Include the
question, observed versus proposed information, source/update sites, interface
example, constraints, dependencies and acceptance/verification criteria. Leave
implementation pending; closed work stays closed unless the owner reopens it.

Proceed with implementation only when the question, chosen representation and
authorization are established. Apply [add-arx-code](../add-arx-code/SKILL.md) for
kernel changes and its documentation completion workflow.

## Maintain the owning record

Inspect the real type and mutation paths, C/C++ linkage, boot availability,
concurrency and object lifetime. Define each value's units, reset/wrap behavior
and exact event: attempts, committed successes, coalescing, copies, consumption
and discards can differ. Reuse existing operation helpers only when their return
semantics match; API success is not proof of delivery or a committed mutation.

Keep custom stats in `debug/kernel/stats/`. Use the release-erasing macros from
DebugRecords.h for storage, update arguments and registration. Guard debug-only
helper declarations, definitions and calls too. Naturally aligned atomic counters
or existing synchronization protect updates; group markers detect interrupted
multiwriter work and do not create a seqlock, payload lock or atomic snapshot.

Wire production publication through the composition code after construction;
selftests can construct temporary managers, so constructors must not register
them. Define the lifetime of borrowed stats, descriptor arrays/strings and roots,
publication/unavailable states, slot reuse identity and quiescent withdrawal before
freeing storage. Keep ordinary downward dependencies; route necessary upward
operations through Dispatcher as specified by add-arx-code.

An ordinary field/counter extends generic `<provider> stats` without a Python
adapter. Finish this stage with documented value semantics, covered mutation
sites and publication/lifetime rules.

## Add collection interpretation when needed

Use `arx_debug_view_t` with an exact root type/size, semantic ID/version and
explicit count/capacity meaning. Register the adapter in
[views/__init__.py](../../../debug/gdb/views/__init__.py). Add parser selectors
only when the question needs them, rather than one command per field or manager.

Read stopped RAM through `Memory`, shared `Budget`/`Result` and the
[framework readers](../../../debug/gdb/framework/readers.py). Charge traversal
work and bytes, including empty-slot scans, and enforce physical bounds, cycle
and depth limits. Validate versions, types, operation identities and owner/slot
incarnations before decoding private or weak relationships. Read large embedded
arrays by member. Preserve gathered rows and explicit issues on partial failure.

Document roots, bounds, sentinels, ordering/ring arithmetic, mask/generation
meanings, weak/owning edges and interrupted-update interpretation beside the
shared layout/stats header and in the guide. Optional correlations must not make
local state unreadable. A future native reader needs the same exact-build
contract; a Python callback is not a kernel dump reader.

Inspection must not execute inferior getters, readiness/delivery methods,
arbitrary expressions, target locks or target mutations. Report unavailable,
partial or truncated evidence instead of filling gaps. Finish this stage when
the stored-state interpretation and bounded failure behavior are explicit.

## Verify and hand off

For code changes, select known useful subsystem/fixture state and compare the
reader with its real owner fields. Cover invalid/unpublished roots, corrupt
counts/links, stale identities, interrupted groups and exhausted budgets as
relevant. Verify debug publication and release exclusion of storage, symbols,
update work and argument side effects, plus affected subsystem behavior. The
current framework requires a matching stopped x86_64 debug ELF; use the guide's
setup rather than assuming a new target is supported.

Keep checks proportional and stop after the selected checks pass. Record missing
prerequisites and actual results; host fixtures are not live GDB or kernel-build
evidence. Resolve tool availability before considering installation. Update the
common guide and affected architecture claims through
[update-kernel-docs](../update-kernel-docs/SKILL.md), then record changes, checks and
remaining work in the authorized ticket. Output either a verified authorized
change with its limits, or the scoped open follow-up ticket described above.

## Existing examples to read selectively

- **Generic counter:** [CredentialDebugStats.hpp](../../../debug/kernel/stats/CredentialDebugStats.hpp),
  [CredentialManager.cpp](../../../kernel/layers/Logic/CredentialManager.cpp) and
  [Dispatcher.cpp](../../../kernel/layers/Dispatcher.cpp) show guarded manager
  storage, decision accounting, grouping and composition publication. Reuse an
  existing field when it already answers the question.
- **Production collection:** [IpcDebugStats.hpp](../../../debug/kernel/stats/IpcDebugStats.hpp),
  [InterProcessCommunicationManager.hpp](../../../kernel/layers/Logic/InterProcessCommunicationManager.hpp),
  [Communication.cpp](../../../debug/kernel/Communication.cpp),
  [communication.py](../../../debug/gdb/views/communication.py) and
  [IPC selftests](../../../kernel/selftests/logic/InterProcessCommunicationManagerTests.cpp)
  show stored pipe state and its subsystem fixtures; the adapter demonstrates
  borrowed descriptor roots, private-type checks, bounded ring/member reads and
  partial results. [The IPC ticket](../../../tickets/closed/add_debug_ipc_socket_views.md)
  records the provider's verification, not proof that a new change is correct.
- **Small controlled fixture:** [foundation.py](../../../debug/gdb/views/foundation.py)
  and [Foundation.cpp](../../../debug/kernel/Foundation.cpp) illustrate shared
  collection readers; these are infrastructure fixtures, not production state.
