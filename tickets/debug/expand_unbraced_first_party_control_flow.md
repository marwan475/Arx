# Expand unbraced first-party control flow

- Type: debug
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:diagnosing-bugs

## Task prompt

The 2026-10-17 code-skill audit at revision `d057ba1` found 50 unbraced
control-flow sites across 22 first-party files. Examples include
`TaskManager::CountTasks`, EventRequestManager's user-output loops, and Klib
error-logger tests. Some loops have an empty line before an unbraced nested
`if`, making the actual body boundary especially difficult to follow.

Expected behavior: first-party control-flow bodies use explicit, expanded
braced blocks matching [kernel.cpp](../../kernel/kernel.cpp) and
[add-arx-code](../../docs/skills/add-arx-code/SKILL.md), with readable newlines
and unchanged behavior.

Follow [TICKET.MD](../TICKET.MD). Read the architecture guides for the affected
areas before changing them. Creation does not authorize analysis or
implementation; use the required diagnosis skill when analysis is requested.

### Reproduce and inspect

The following locations come from the audit; line numbers apply to `d057ba1`.
Re-find the surrounding symbols if intervening changes move them. These are
source-level formatting findings, not demonstrated runtime failures.

| File | Unbraced control-flow lines |
| --- | --- |
| [arch/x86_64/arch_entry.c](../../arch/x86_64/arch_entry.c) | 155, 157, 374, 407 |
| [arch/x86_64/arch_paging.c](../../arch/x86_64/arch_paging.c) | 542, 740, 1083, 1085 |
| [arch/x86_64/lapic.c](../../arch/x86_64/lapic.c) | 76 |
| [kernel/layers/Logic/CredentialManager.cpp](../../kernel/layers/Logic/CredentialManager.cpp) | 74, 179, 213 |
| [kernel/layers/Logic/InterProcessCommunicationManager.cpp](../../kernel/layers/Logic/InterProcessCommunicationManager.cpp) | 691 |
| [kernel/layers/Logic/Scheduler.cpp](../../kernel/layers/Logic/Scheduler.cpp) | 152, 371, 752 |
| [kernel/layers/Logic/SynchronizationManager.cpp](../../kernel/layers/Logic/SynchronizationManager.cpp) | 169, 171, 223, 225 |
| [kernel/layers/Logic/VirtualFileSystem.cpp](../../kernel/layers/Logic/VirtualFileSystem.cpp) | 3158 |
| [kernel/layers/Request/EventRequestManager.cpp](../../kernel/layers/Request/EventRequestManager.cpp) | 250, 273, 357, 366 |
| [kernel/layers/Request/SystemRequestManager.cpp](../../kernel/layers/Request/SystemRequestManager.cpp) | 243 |
| [kernel/layers/Request/VfsRequestManager.cpp](../../kernel/layers/Request/VfsRequestManager.cpp) | 430, 1985 |
| [kernel/layers/Resource/ProcessManager.cpp](../../kernel/layers/Resource/ProcessManager.cpp) | 548, 1511, 1524 |
| [kernel/layers/Resource/TaskManager.cpp](../../kernel/layers/Resource/TaskManager.cpp) | 21, 497 |
| [kernel/layers/Resource/TimeManager.cpp](../../kernel/layers/Resource/TimeManager.cpp) | 49 |
| [kernel/platform/acpi/acpi.c](../../kernel/platform/acpi/acpi.c) | 61 |
| [kernel/selftests/platform/klibtests.c](../../kernel/selftests/platform/klibtests.c) | 253, 261, 269, 318, 325, 334, 346, 360 |
| [kernel/selftests/platform/memorytests.c](../../kernel/selftests/platform/memorytests.c) | 159 |
| [kernel/selftests/poststartkernel/schedulertests.cpp](../../kernel/selftests/poststartkernel/schedulertests.cpp) | 366 |
| [kernel/selftests/poststartkernel/smpfixtures.hpp](../../kernel/selftests/poststartkernel/smpfixtures.hpp) | 13 |
| [kernel/selftests/poststartkernel/smpselftests.cpp](../../kernel/selftests/poststartkernel/smpselftests.cpp) | 447 |
| [kernel/selftests/poststartkernel/taskcontexttests.cpp](../../kernel/selftests/poststartkernel/taskcontexttests.cpp) | 201 |
| [klib/klib.c](../../klib/klib.c) | 24, 261 |

During analysis, inspect each listed body, including nested `if`/`else`
associations. Re-scan first-party C/C++ sources under `arch/`, `kernel/`, `klib/`,
`boot/`, and `initramfs/` for equivalent sites; regex results alone are not a
complete parser. Mask comments and literals and balance multiline conditions.
Exclude vendor uACPI, Flanterm, printf, khashl/khashp, and `boot/limine.h`.
Treat macro continuations and `do { ... } while (0)` tails separately rather
than reporting them as ordinary missing-brace sites.

The initial scan's completeness beyond these 50 sites is unknown. Confirm the
inventory and execution scope during diagnosis. Insert braces around the existing
body without changing loop coverage, `else` association, early returns, or
variable scope/lifetime. Inspect those semantics explicitly: adding braces is
not always equivalent to a whitespace-only change. Preserve comments and add
blank lines where needed for the corrected block to match `kernel.cpp`.

Keep broader spacing work in
[its separate ticket](improve_dense_kernel_code_spacing.md), compressed lambdas
in [their ticket](expand_compressed_selftest_lambdas.md), and architecture changes
in the dependency tickets. Overlapping files should use the latest source, not
outdated audit line numbers. Select affected build/syntax checks during analysis;
no runtime failure or new test scenario was established by this audit.

## Acceptance criteria

- [ ] All 50 audited sites are accounted for, including sites changed by other work.
- [ ] The agreed first-party scope has explicit braced control-flow bodies; candidate scan results are reviewed for macro and syntax false positives.
- [ ] Each change preserves statement ordering, branch association, loop coverage, and declaration lifetime.
- [ ] Formatting follows `kernel.cpp`, with readable blocks and blank-line separation; bundled third-party code remains untouched.
- [ ] Appropriate syntax/build verification and diff checks are selected and recorded, and documentation impact is reported without unsolicited guide edits.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 because expanded,
human-readable control flow is an explicit owner requirement. The initial
inventory is 50 sites in 22 files; analysis and execution have not started.
