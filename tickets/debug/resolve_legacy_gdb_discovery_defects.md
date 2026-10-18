# Resolve legacy GDB symbol and manager discovery defects during cutover

- Type: debug
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:diagnosing-bugs

## Task prompt

Record and resolve confirmed code-level defects in the current GDB extension's
discovery helpers. The replacement framework must not copy these assumptions,
and removal of the old script must have a verified defect disposition. This is
one grouped legacy-discovery debug ticket, not an extra feature or authorization
to repair all old commands immediately.

Follow [the workflow](../TICKET.MD), the required diagnosing-bugs skill during
analysis, and applicable repository skills. Parent:
[framework planning](../closed/plan_gdb_extension_tickets.md).
Related: [foundations](../closed/build_debug_framework_foundation.md),
[cutover](../feature/migrate_debug_commands_and_document_framework.md),
[legacy inventory](../feature/migrate_debug_commands_and_document_framework.md#legacy-command-inventory).
This ticket starts open; diagnosis/implementation retains its own owner gate.

### Observed versus expected behavior

1. `ArxPlatform._extract_first_hex()` returns `None` for a string containing a
   valid hexadecimal symbol address. Its successful integer-return block is
   misplaced after `_parse_and_eval_with_language()` rather than in the helper.
   Expected: return the matched integer address or the documented not-found value.
2. `_parse_and_eval_with_language()` restores `c` for a previous language string
   containing `auto; currently c++`: the substring test for `currently c` also
   matches C++. Expected: preserve the actual prior GDB language selection,
   including auto mode, on success and failure.
3. Resource raw fallbacks and `arx-rsrc` interpret pointer slots 1/3/4 as
   process/task/initramfs. Current Resource caps place those at 2/5/6. VFS raw
   fallback reads Logic slot 1, currently ProcessController; VFS is slot 2.
   Expected: resolve the actual typed manager or report unavailable, never label
   another manager's address as the requested object.
4. Process/task array fallbacks cast manager base addresses as arrays even though
   current fields precede `Processes`/`Tasks`; task capacity fallback is 64 while
   `MAX_TASKS` is 128. Expected: use verified type/layout/capacity or fail explicitly.

Items 1–2 were reproduced by evaluating the AST-loaded ArxPlatform class with a
stub GDB interface during parent inventory. Items 3–4 are current-source layout
mismatches. No live target session was run, so actual fallback frequency, normal
typed-path masking and user-visible runtime consequences remain unverified.
Do not claim kernel memory corruption or a specific observed boot failure.

### Reproduction and investigation

Read [gdb_extention.py](../../scripts/gdb_extention.py): ArxPlatform helpers,
ArxResourceManagers, VFS discovery, array fallbacks and ArxResourceDebugCommand.
For the pure helper reproduction, extract the ArxPlatform class through Python
`ast`, execute only that class with `re` and a stub `gdb` object, and evaluate:

```text
_extract_first_hex("Symbol at 0xffffffff80001000.")
  observed: None; expected: 0xffffffff80001000 as an integer
stub show language -> auto; currently c++
_parse_and_eval_with_language("platform", "c")
  observed commands: set language c, set language c
  expected restoration: previous auto selection
```

Do not source the whole extension into host Python: it requires GDB and registers
commands. Compare current declarations in
[ResourceLayerFactory.hpp](../../kernel/layers/Resource/ResourceLayerFactory.hpp),
[LogicLayerFactory.hpp](../../kernel/layers/Logic/LogicLayerFactory.hpp),
[ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp) and
[TaskManager.hpp](../../kernel/layers/Resource/TaskManager.hpp).
Use actual GDB/DWARF field offsets if runtime path verification is needed, rather
than assuming C++ byte layout from pointer indices alone.

### Resolution boundaries and remaining decisions

The owner plans to remove the legacy script after parity. During analysis decide
whether an interim repair is actually needed for a current investigation or
whether verified removal plus safe replacement resolves the defect. Do not
indefinitely support an obsolete resolver or add raw-offset compatibility to the
new framework. If retained before cutover, minimal repairs need a reproduction
and relevant checks; if removed, record replacement safety and decommission
evidence for all four defects. Do not modify Request-layer managers, introduce
new feature views or implement dump mechanics.

## Acceptance criteria

- [ ] Analysis reproduces/classifies the helper defects and layout mismatches, distinguishing source evidence from live-target observations and unknowns.
- [ ] Each defect has an explicit disposition: verified minimal repair if needed, or verified legacy removal with a safe replacement path.
- [ ] Retained discovery never returns a missing success value, changes the prior GDB language setting, labels stale slots as another manager or guesses invalid array layout/capacity.
- [ ] New framework discovery does not copy these defects or execute inferior getters; unavailable symbols/types produce explicit failure.
- [ ] Verification and remaining runtime limitations are recorded; related migration/guide references reflect the disposition without unrelated legacy refactoring.

## Verification approach

Reproduce pure helpers with isolated controlled stubs for valid/missing hex,
auto/C/C++ and evaluation exceptions. If an interim repair is chosen, verify the
retained paths in a matching-symbol x86_64 GDB session and controlled missing-type
conditions without executing inferior functions. For cutover resolution, verify
the old script/registrations are gone and the new registry discovers typed
records without guessed member slots, across ELF/inferior changes and absent
providers. A live normal path passing does not establish broken fallbacks are safe.

## Work log

### 2026-10-18 — Created from framework inventory

P2: confirmed code-level discovery defects merit tracking, while their live-target
impact remains unknown and the owner already plans replacement. Grouped into
one debug ticket to avoid adding separate speculative repair workloads.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
