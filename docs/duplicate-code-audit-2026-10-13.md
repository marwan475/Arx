# Duplicate-code audit

Reviewed October 13, 2026, against the current working tree. aarch64 files remain excluded, following the kernel audit's scope. This is an audit and refactoring recommendation; no implementation files were changed.

I scanned 97 C/C++/header/assembly files in the common kernel, x86_64, and klib paths, totaling 21,373 physical lines at the scan snapshot. Vendored uACPI, Flanterm, printf, and selftests were excluded from the quantitative scan. Relevant selftests and API callers were inspected separately for context.

The exact-match scan normalized whitespace, removed short brace-only lines, and ignored comment/include lines, then compared windows of ten remaining lines. It found **119 distinct repeated windows**. Windows overlap, so this is not 119 independent findings or a duplicated-line percentage. Identifier-renamed and behaviorally overlapping implementations were checked manually. Files continued changing during the review, so the numbers and line references describe observed snapshots.

**Ten actionable duplication groups:** three high-priority, five medium-priority, and two low-priority cleanup opportunities. These are refactoring priorities, not ten additional kernel bugs.

## 1. High: multiple implementations of process-memory copying

Examples:

- [klib.c](/C:/Users/marwa/develop/Arx/klib/klib.c:29): safe_read_user_locked / safe_write_user_locked, with effective permission validation and a pinned access context.
- [VirtualMemoryManager.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/VirtualMemoryManager.cpp:117): CopyFromUser / CopyToUser, with direct virtual memcpy and legacy validation.
- [VirtualMemoryManager.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/VirtualMemoryManager.cpp:296): WriteAnonymousUser, with a separate region/backing-based permission test and copy loop.
- [ProcessManager.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/ProcessManager.cpp:21): copy_to_process_virtual, with another page-splitting and HHDM copy loop for stack construction.

This is already behaviorally divergent. The legacy VMM user validator accepts the kernel half and checks only translation for low addresses. The newer klib path rejects the kernel half and checks page permissions. Stack construction translates an address including its offset and then adds that offset again. WriteAnonymousUser supports only tracked anonymous backing, unlike the general copy path.

Recommended boundary: one page-chunk traversal/translation primitive plus explicit policies for reading userspace, writing userspace, and initializing an inactive process's memory. Preserve the different ownership and permission contracts; do not blindly replace stack initialization with a helper that assumes the current process or already-final permissions. Move all syscall callers to one safe user-copy family, including bounded string copying. Keep anonymous-page residency/ownership checks in ProcessMemoryManager when those are part of syscall semantics.

The read/write direction-specific loops inside klib are small and understandable; the larger problem is competing validation and lifetime contracts across subsystems.

## 2. High: vmalloc rollback and normal free duplicate backing teardown

Examples: [klib.c rollback_vmalloc](/C:/Users/marwa/develop/Arx/klib/klib.c:291) and [vfree](/C:/Users/marwa/develop/Arx/klib/klib.c:408).

Both walk a mapped virtual interval, translate pages, inspect buddy metadata, detect allocation heads, infer the original order, free physical blocks, and advance by the inferred block size. A ten-significant-line portion of this teardown matches exactly. Their surrounding region handling differs because one receives a partial extent and the other discovers a region.

Both copies also share the kernel audit's free-before-unmap problem. Patching only one leaves the other capable of returning still-mapped pages to PMM.

Recommended boundary: a common ownership-aware teardown routine taking the address space and mapped extent, with explicit backing-allocation records. Remove mappings and complete shootdowns before returning frames. Keep rollback and public vfree as small adapters with their respective region/lifetime checks. Do not infer allocation ownership solely from whatever PTEs happen to exist at free time.

## 3. High: FD lookup, validation, and replacement are copied across VFS syscalls

Examples: [read](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:836), [write](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:876), [pread64](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:916), [readv](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:1008), and [fstat](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:2027).

The scan found one identical ten-significant-line validation sequence in **17 locations** in VfsRequestManager.cpp: get current process, reject absent process, check FD bounds/table, obtain descriptor, and reject an empty descriptor. Nearby handlers also duplicate capability checks and file extraction.

HandleDup2Request and HandleDup3Request separately repeat capacity growth, close-and-clear of a destination descriptor, retain of the source file, and installation of the replacement. HandleFcntlRequest/HandleDupRequest also implement related retain/install logic.

Recommended boundary: put synchronized FD lookup/retain and descriptor installation/replacement into ProcessManager's FD API. Return a retained file handle with an explicit release contract, not a raw descriptor pointer. Reuse that API in small syscall-specific handlers. Keep dup2's oldfd==newfd success, dup3's oldfd==newfd EINVAL, and CLOEXEC differences explicit; these semantic differences are intentional.

This consolidation would give the lookup/close/table-growth race from the kernel audit one place to be fixed. A convenience helper that merely returns the existing borrowed pointer would reduce lines without solving that problem.

## 4. Medium: pathname preparation is repeated in Request and Logic

Examples:

- Request: [HandleMkdiratRequest](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:766), [HandleUnlinkatRequest](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:1502), and [HandleMknodatRequest](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:2919).
- Request helpers: [resolve_open_start_path](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:252) and [resolve_relative_base_for_dirfd](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:310).
- Logic: [Mkdir](/C:/Users/marwa/develop/Arx/kernel/layers/Logic/VirtualFileSystem.cpp:1566), [Unlink](/C:/Users/marwa/develop/Arx/kernel/layers/Logic/VirtualFileSystem.cpp:1605), [Mknod](/C:/Users/marwa/develop/Arx/kernel/layers/Logic/VirtualFileSystem.cpp:1768), and [TruncatePath](/C:/Users/marwa/develop/Arx/kernel/layers/Logic/VirtualFileSystem.cpp:1807).

Request handlers repeatedly copy a pathname, expand AT_FDCWD using cwdPath, select a dirfd base, and distinguish EBADF from ENOTDIR. Logic operations separately repeat absolute-path copying or BuildAbsolutePathFromDentry plus JoinPath. The old/new path branches of Rename and Link duplicate the same preparation twice. The scan found exact repeated windows in four single-path Logic operations, and in the paired-path operations.

Recommended boundary: a Request helper for importing a user pathname and selecting its starting path, and a Logic helper for producing the backend pathname. Keep these two helpers separate because they own different policies. Pass flags explicitly for empty paths, symlink following, and directory requirements. Avoid making syscall errno selection depend on an undifferentiated false return.

## 5. Medium: process/task initialization and reset logic is maintained in several places

Examples:

- Process: [constructor](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/ProcessManager.cpp:73), [AllocateProcessUnlocked](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/ProcessManager.cpp:118), and [FreeProcess](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/ProcessManager.cpp:235).
- Task: [constructor](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/TaskManager.cpp:65), [AllocateTaskUnlocked](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/TaskManager.cpp:103), and [FreeTask](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/TaskManager.cpp:203).
- Task creation: [CreateKernelTask](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/TaskManager.cpp:136) and [CreateUserBootstrapTask](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/TaskManager.cpp:171).

Process initialization repeats defaults for parent, exit state, memory counters, timers, task list, descriptors, umask, and cwd. Task initialization repeats context, stack, links, syscall/preemption state, and the new sleep flags. Free/reset paths do not all clear the same fields; for example, task reuse clears sleep/syscall/preemption fields in AllocateTaskUnlocked while FreeTask clears only a subset. That is not necessarily an active bug, but makes the state of an unallocated slot depend on how it was reached.

Both task creation functions also repeat stack allocation, manager lock acquisition, slot allocation, failure cleanup, and context setup.

Recommended boundary: explicit ResetProcessSlot and ResetTaskSlot helpers called only after releasing owned resources and quiescing execution. Keep slot ID and allocated-state transitions explicit. Share task stack/context preparation, leaving kernel entry arguments and user-launch fields in small specialized adapters. Do not use blanket memset in place of ownership-aware teardown.

## 6. Medium: MADT parsing repeats the record walker four times

Examples: [get_lapic_base_addr](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_acpi.c:4), [get_lapics_from_madt](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_acpi.c:56), [get_ioapic_from_madt](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_acpi.c:137), and [get_iso_overrides_from_madt](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_acpi.c:189).

Each parser computes the table end, walks variable-length records, rejects short headers/overruns, selects record types, checks type-specific sizes, and advances by entry length. Header validation is not uniformly located: the LAPIC collector relies on the preceding base-address scan for it.

Recommended boundary: one checked MADT iterator that validates the enclosing table and generic record bounds. Keep record-specific size checks and handling in separate functions. This reduces the number of places to update malformed-table handling and makes the dependency on another parser's validation unnecessary.

## 7. Medium: PCI reads repeat ECAM address calculation

Examples: [pci_ecam_read8](/C:/Users/marwa/develop/Arx/arch/x86_64/pci.c:21) and [pci_ecam_read32](/C:/Users/marwa/develop/Arx/arch/x86_64/pci.c:61).

Both repeat region/output validation, bus bounds, the bus/device/function shifts, offset addition, and mapped-size checks. Only access width and alignment differ. Both also share the nonzero-start-bus issue identified by the kernel audit.

Recommended boundary: a checked pci_ecam_address(region, bus, device, function, offset, width, alignment) helper. Keep the actual volatile 8-bit and 32-bit loads separate so the access width is explicit. Correct the mapped-region base convention once and cover it through both accessors.

## 8. Medium: MMIO mapping policy is independently assembled at several call sites

Examples: [lapic_init](/C:/Users/marwa/develop/Arx/arch/x86_64/lapic.c:24), [ioapic_init](/C:/Users/marwa/develop/Arx/arch/x86_64/ioapic.c:166), [arch_time_init](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_time.c:88), and [uacpi_kernel_map](/C:/Users/marwa/develop/Arx/kernel/platform/acpi/acpi.c:26).

These paths each assemble mapping flags and deal with physical alignment, virtual selection, and mapping verification. APIC initialization maps only when it cannot translate an existing address; HPET maps and then verifies translation; the uACPI callback reserves a separate range and maps without device-cache flags, even when used for ECAM.

Recommended boundary: a common MMIO mapping service with cache policy, failure reporting, existing-alias checks, and ownership. Keep firmware-table RAM mappings a separate policy. Preserve each device's register alignment and access width. Also cache each CPU's initialized LAPIC virtual base instead of recomputing the HHDM conversion in timer setup, EOI, and send_ipi.

This is policy duplication, not an exact textual clone: superficially similar setup has already accumulated different behavior.

## 9. Low: x86 register and TSS helpers have duplicate definitions

Examples: [arch_syscall.c](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_syscall.c:30), [lapic.c](/C:/Users/marwa/develop/Arx/arch/x86_64/lapic.c:7), and [arch_init.c](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_init.c:17).

rdmsr/wrmsr occur in both syscall and LAPIC code. read_rsp and set_tss_rsp0 occur in syscall and architecture initialization. The latter duplicates the split of a 64-bit stack address into the packed TSS fields.

Recommended boundary: an internal x86 primitives header for typed MSR/register access, and one TSS stack setter. Retain explicit compiler barriers appropriate to each operation. This is a small cleanup with less benefit than memory and FD consolidation.

## 10. Low: Linux ABI layouts and errno values have multiple sources

Examples: [VfsRequestManager.cpp linux_timespec_t](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:10), [TimeManager.hpp](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/TimeManager.hpp:7), and [RequestLayerFactory.hpp errno constants](/C:/Users/marwa/develop/Arx/kernel/layers/Request/RequestLayerFactory.hpp:21).

VFS defines its own two-int64 timespec for stat fields while TimeManager defines the same wire layout under another name. RequestLayerFactory.hpp owns a subset of errno constants, VfsRequestManager.cpp adds local constants, and newer managers return numeric negative literals. Internal VFS/resource errors also repeat many of those numbers under different names.

Recommended boundary: a small Linux ABI header for actual wire structures and named errno values, separate from the request-layer factory and implementation includes. Add size/offset assertions to the shared structures. Keep internal errors distinct where they have an intentional translation contract. Numeric literals and equivalent layouts are not proof of a runtime ABI defect, but increase the cost of review and migration.

## Duplication that should generally stay

- Resource capability wrappers and C/C++ bridge thunks implement the repository's layer boundaries. Their similarity alone is not a reason to bypass the architecture or replace them with opaque macros.
- arch_map/unmap/protect have related setup code, but already share the page-table walker and leaf callbacks. A small operation builder might reduce setup; replacing readable wrappers is a lower priority than fixing their error propagation.
- Scalar/vector/positioned I/O entry points intentionally differ in ABI and error behavior. Keep those entry points; share lookup/lifetime and transfer primitives underneath.
- Unsupported syscall stubs and generated ISR stubs are intentional repetitive interfaces.
- Test setup repeats independently exercised scenarios. Do not merge tests merely to lower clone counts or accidentally remove independent assertions.
- Third-party code should retain upstream structure unless there is a separately justified fix.

## Suggested sequence and verification

1. Consolidate user-copy policy and fix process-stack arithmetic; verify kernel-pointer rejection, read-only/PROT_NONE destinations, cross-page copies, and inactive-address-space initialization.
2. Unify vmalloc teardown with explicit ownership and correct shootdown-before-reuse ordering; verify both normal free and partial-allocation rollback.
3. Centralize retained FD lookup/replacement, then simplify the repetitive VFS syscall guards. Exercise concurrent close/read and dup2/dup3 identity/CLOEXEC cases.
4. Extract Request/Logic path-preparation helpers with explicit flags. Verify absolute/relative paths, AT_FDCWD, invalid/non-directory dirfds, and empty-path/symlink behavior.
5. Add lifecycle reset helpers, checked MADT iteration, checked ECAM addressing, and common MMIO policy in separate changes.
6. Finish with x86 primitive and ABI-header cleanup.

No compiler/runtime tests were run for this audit, and no refactor was applied. The evidence is a normalized-source scan plus manual comparison. The working tree continued changing, including migration of time syscalls to klib user-copy helpers; recheck callers against a frozen revision before implementing the recommendations.
