# Add initramfs and complete supported VFS namespace debug views

- Type: feature
- Priority: P1
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Expose filesystem statistics, initramfs archive entries and VFS namespace
structure through DebugManager. The owner wants filesystem contents, not only
counts. Existing `arx-vfs` lists mounts and cached paths; that cache is not a
complete filesystem listing. Diagnose missing/hidden paths, mount coverage,
runtime rename/unlink behavior and differences between archive contents and the
visible mounted namespace without executing VFS enumeration methods.

Follow [the workflow](../TICKET.MD), required grilling and repo skills. Parent:
[planning](../closed/plan_gdb_extension_tickets.md). Depends on
[foundations](../closed/build_debug_framework_foundation.md); use shared file identities
from [process/resource views](../closed/add_debug_process_resource_views.md) when available.
See [assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment) and
[Logic documentation](../closed/document_logic_layer.md). This ticket starts open
and requires its own analysis/execution gate.

### Scope and internal milestones

1. Define subsystem-owned VFS/initramfs/resource-filesystem debug headers and
   agreed maintained counts/failures/current namespace values. Publish typed
   collection roots/lengths through DebugManager using production construction
   wiring. No uniform manager layout or Request provider is required.
2. Migrate the existing initramfs array/count, exact-path lookup and path/size/data
   information; add mode/UID/GID and an interpreted path hierarchy. Preserve
   capacity validation, explicit bounds, string limits and unavailable archive
   state. Archive hierarchy is derived from flat path names, not a stored tree.
3. Migrate accurately named VFS mount/cache views: namespace/root mount, mount
   flags/parent/mount point, filesystem identity/type/flags, root inodes, hash-cache
   occupancy and dentry ancestry. Include overlay, retired/unlinked/renamed entries
   and namespace generation where meaningful. Bound bucket scans, not merely the
   number of occupied results; surface skipped/unreadable data.
4. Add `contents` for the complete visible namespace of currently supported
   filesystem backends. Interpret retained backend/InitRamFS data plus runtime
   overlays and mount rules, honoring unlink/rename, shadowing and mount coverage.
   Cached entries alone must never be labeled complete. Inventory actual backends
   in analysis and report unsupported backend/truncated/incomplete branches
   explicitly. Do not call ReadDirectory/Lookup/Mount, mutate cursors/cache or add
   a second full tree unless a separately justified debug representation is agreed.
5. Supply reusable path/mount/file identities to descriptor/IPC/event views.
   Default contents show structure/metadata. Any requested payload detail must be
   explicit, byte-bounded and separate from namespace enumeration. Specify what
   roots and backend interpretation a future native dump reader needs; Python
   must not be the only definition of namespace visibility.
6. Add adapters under `debug/gdb/views/`, guide examples/limits and old
   VFS/initramfs coverage entries for [cutover](../feature/migrate_debug_commands_and_document_framework.md).

### Proposed interface and illustrative output

```text
arx-debug initramfs contents --path / --limit 64
  /bin/test mode=0755 uid=0 gid=0 size=4096 data=<addr>
arx-debug vfs mounts
  / type=cpio parent=<root> flags=<flags>
arx-debug vfs cache --limit 64
  scope=cached-only /tmp/open-file inode=<addr> unlinked=true
arx-debug vfs contents --path / --depth 4 --limit 100
  /bin/test file size=4096 source=cpio
  /tmp directory source=runtime
  completeness=complete-for-supported-backends
```

Examples are illustrative; grammar follows foundation analysis. Tracking/data
and registration compile out in release. All traversal is read-only, without
target locks/functions, and reports partial update/cycle/null-root status.
General filesystem implementation changes, unsupported-backend emulation,
Request-layer tracking and dump mechanics are excluded.

### Sources and design unknowns

- [InitRamFileSystemManager.hpp](../../kernel/layers/Resource/InitRamFileSystemManager.hpp),
  [implementation](../../kernel/layers/Resource/InitRamFileSystemManager.cpp):
  `Archives`, `ArchiveCount`, capacity and parsed entry metadata.
- [ResourceFileSystem.hpp](../../kernel/layers/Resource/ResourceFileSystem.hpp),
  [implementation](../../kernel/layers/Resource/ResourceFileSystem.cpp): backend
  path normalization, resource nodes and archive-derived enumeration semantics.
- [VirtualFileSystem.hpp](../../kernel/layers/Logic/VirtualFileSystem.hpp),
  [implementation](../../kernel/layers/Logic/VirtualFileSystem.cpp):
  `Namespace`, `MountListHead`, `DentryCache`, retired/overlay roots and link rules.
- [khashp.h](../../klib/khashl/khashp.h): bucket/occupied bitmap layout.
- Resolve backend completeness rules, overlay deletion/rename semantics,
  mount/path limits, snapshot caveats and private type exposure during analysis.
  Do not assume dentry parent links provide an iterable child tree.

## Acceptance criteria

- [x] Recheck the recorded cleanup failure and document the outcome; the owner accepted stopping when it did not reproduce.

- [x] Maintained stats and typed roots cover VFS/initramfs/resource backend state without release work/storage.
- [x] Archive listing/selection/hierarchy preserves old information and adds bounded ownership/mode detail.
- [x] Cache and mount views preserve useful old output and explicitly distinguish cached state, overlays, retired/unlinked entries and readable counts.
- [x] Supported visible namespace contents are reconstructed from backend plus runtime/mount state; incomplete/unsupported branches never claim completeness.
- [x] Invalid paths, count overflow, hash buckets, parent/mount cycles, missing providers and unreadable strings terminate with useful status without target calls.
- [x] Native-reader path/visibility rules, shared identities, guide and coverage ledger are updated; dump mechanics remain absent.

## Verification approach

Use [InitRamFileSystemManagerTests](../../kernel/selftests/resource/InitRamFileSystemManagerTests.cpp),
[ResourceFileSystemTests](../../kernel/selftests/resource/ResourceFileSystemTests.cpp),
[VirtualFileSystemTests](../../kernel/selftests/logic/VirtualFileSystemTests.cpp)
and [VFS integration](../../kernel/selftests/poststartkernel/poststartvfstests.cpp).
Compare archive enumeration with controlled known paths, including entries never
looked up into the VFS cache. Exercise runtime mkdir/rename/unlink, an open
unlinked file, mount shadowing and metadata changes. Inspect before root mount,
with empty archives and with corrupt counts/strings/hash/mount links in safe
fixtures. Confirm inspection does not change cache state or file positions.
Build/check debug and release for all added update/registration sites.

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

P1: the owner explicitly requested complete supported VFS contents. Combines
namespace, backend and archive interpretation into one shared path workload.

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

The owner selected VFS/initramfs contents. Read the ticket, workflow, required
grilling skill and repository code/documentation skills. Delegated read-only
fact finding for namespace visibility and archive/backend semantics as required
by grilling. Human-readable output and the existing proportional verification
stopping rule remain in effect. No implementation or checks have started;
status remains open pending design confirmation.

### 2026-10-20 — Analysis and plan; owner design confirmation

The owner answered “yes to all” to separate archive/visible/cache views,
human-readable metadata trees, no payload capture in listings, and no recursive
symlink following. The owner also confirmed full reconstruction for the shipped
cpio backend, explicit incomplete branches for callback-only custom backends,
and preservation of stored names/raw archive types separately from actual
VFS-visible semantics. Shared understanding is confirmed. No implementation or
builds have started; execution awaits the owner's separate instruction.

#### Source findings and visibility contract

- InitRamFileSystemManager owns Archives[256] and ArchiveCount. Each entry has
  path, current size/data, raw mode, UID and GID. Read these records directly,
  including entries never looked up into the VFS cache. Loading strips leading
  ./ components then leading slashes, but not internal dot/separator/trailing
  slash anomalies. Preserve stored names and flag ambiguity; do not silently
  canonicalize archive records into different names.
- ResourceFileSystem's shipped cpio backend derives hierarchy from archive
  paths. Exact entries take precedence; otherwise descendant prefixes imply
  directories. Lookup exposes directories or regular files even when raw
  archive mode encodes another type. Implicit directories use backend defaults.
  Archive size/data are mutable through writes and shrinking truncation, so
  output describes current parsed state, not an immutable original image.
- resource_fs_t retains the actual InitRam manager used by that mount;
  resource_node_t stores filesystem/path/directory/cached-size state. Alternate
  archive managers are supported. There is no complete backend-node registry,
  and transient wrappers must not be mistaken for all filesystem contents.
- VFS authoritative roots are Namespace.rootMount, MountListHead, DentryCache,
  OverlayDentryHead and RetiredDentries. The hash cache is lazy and incomplete.
  Overlay lookup precedes cache lookup. Publish borrowed typed roots and verified
  resource/symlink operation identities; decode private data only after matching
  supported identities. No shadow namespace tree is needed.
- Namespace identity includes mount plus dentry. Mount crossings match parent
  mount and mount-point dentry, and bind roots can be interior dentries. Move
  mounts change parent/point. Apply shadowing/crossing rules with bounds and
  cycle checks. Keep mount and filesystem flags distinct.
- Unlinked/retired entries and rename forwarding records support existing open
  file identities. They are visible in diagnostic cache/retired output, but not
  independent current children. Bound forwarding/parent chains. Inode numbers
  alone do not identify path occurrences, particularly with hard/bind links.
- khashp capacity is 1<<bits when bucket storage exists; count is occupancy.
  Used bitmap words describe occupied buckets; bucket stride is key_len+val_len.
  Validate dimensions and address arithmetic. Charge every bucket scanned,
  including empty buckets, separately from displayed paths and copied bytes.
- Only cpio is a production registered backend found in this review. Custom
  filesystem callbacks/private data do not provide a safe universal enumeration
  format. Show their known observed children with explicit unavailable/incomplete
  backend branches; never infer completeness from cache occupancy.

#### Implementation plan and affected code

1. Before code changes, read kernel.cpp and the affected Resource, Logic,
   Dispatcher and kernel-architecture guide sections. Apply repository code and
   documentation skills, preserve readable formatting and layer boundaries.
2. Add debug-only subsystem-owned records/native contracts for InitRam manager,
   ResourceFileSystem and VirtualFileSystem. Expose authoritative collections,
   capacities, construction/readiness and mutation-group markers through normal
   Dispatcher composition. Temporary fixture managers do not register globally;
   explicit typed-instance inspection can select their records safely.
3. Instrument owning public operations once, avoiding thunk/helper double counts.
   Cover archive load outcomes and current entry counts; backend mount/lookup/
   enumeration/read/write/mutation outcomes and returned byte totals where
   useful; VFS mount/namespace/cache mutation outcomes and retained current roots.
   Successful EOF/zero-byte I/O is not failure. Namespace generation and grouped
   mutations describe transitions, not an atomic cross-provider snapshot.
   All added storage, registration and argument evaluation disappear in release.
4. Add initramfs contents and exact-path selection, deriving readable ancestor
   directories without target calls. Show raw mode/UID/GID, current size and data
   identity; distinguish explicit entries from inferred directories. No payload
   extraction is included. Bound strings, archive count and hierarchy work.
5. Add vfs mounts, cache and contents adapters under debug/gdb/views. Reuse
   generic budgets and existing process/file identities. Support --path subtree
   selection (root by default) and explicit typed VFS-instance selection for
   controlled fixtures, with duplicate/incompatible options rejected. Settle
   routine option names consistently with existing numeric-root conventions.
6. Reconstruct supported visible contents from each mount's actual backend
   archive plus runtime overlays/current cached metadata and mount rules. Track
   mount/dentry/path identities separately from inode identity, honor unlinked/
   rename forwarding and shadowing, and never recursively follow symlinks.
   Distinguish cache snapshots from backend metadata when they differ. Flag
   ambiguous archive paths, unreadable records, unknown backends, missing roots,
   cycles and all budget truncation explicitly. Show supported completeness only
   when all required source traversal finished for that selected subtree.
7. Publish native-reader layout/lifetime/precedence/path/mount semantics beside
   debug headers and in GdbCommands.md. Update existing Resource/Logic/Dispatcher
   architecture descriptions and the legacy arx-initramfs/arx-vfs coverage ledger.
   Keep legacy commands for cutover. No Request provider, generic backend
   emulation, second full tree, dump mechanics or filesystem behavior changes.

#### Focused verification and stopping rule

- One focused host fixture suite: uncached archive paths/implicit directories,
  exact-entry precedence/raw versus backend kinds, alternate archive sources,
  overlays/retired/rename state, mount shadow/bind/move identities, unsupported
  backend branches, malformed strings/counts/hash dimensions/links, empty sparse
  buckets and independent scan/display/byte/depth limits. Check explicit partial
  results instead of fabricating a complete listing.
- Build one x86_64 debug and one release image after final runtime changes and
  run focused compile-out/symbol/native-header checks on added sites/records.
- One relevant QEMU/GDB session uses existing archive enumeration/bounds and
  Resource backend fixtures, plus the existing test_vfs_backend_syscalls local
  VFS fixture for rename/unlink/bind/move and custom-backend incompleteness.
  Inspect before root publication and populated cpio contents where relevant;
  compare known uncached paths and source identities. Confirm read-only
  inspection does not change cache counts or directory/file cursors.
- Stop once these checks pass. Repeat/broaden only for a new change, failed
  relevant check or concrete unresolved risk, recording why. Do not add repeated
  boots, unrelated suites or baseline investigations for extra confidence.

Principal risks are backend-versus-archive naming/type differences, lazy cache
coverage, mutable/retired metadata and mount-relative identity. These have explicit
interpretation and partial-state rules above. No design decisions remain open.
P1 is unchanged. Implementation, focused verification and documentation remain;
status is analyzed pending the owner's execution instruction.

### 2026-10-20 — Execution authorized

The owner instructed “go”. Moved to in_progress and started the agreed work.
Existing generated-bytecode changes are preserved.

### 2026-10-20 — Ready for owner review

Completed archive, Resource backend and VFS owner records/counters and production
registration. Added initramfs contents/entry and vfs mounts/cache/contents views,
with bounded path/instance selection, readable metadata, native layout contracts
and explicit completeness. Resource callback identities are verified before
private-node decoding. Retained layouts now live in single-sourced owning layer
headers, preserving their layout and runtime behavior.

Visible native cpio contents include uncached archive paths, inferred directories,
current metadata, overlays and mount crossings. Alternate archive managers are
followed through the actual backend node. Archive root markers and raw mode/type
are distinguished from backend semantics. Cache/retired output retains ancestry,
unlinked and rename identities; custom callback-only backends remain explicitly
incomplete. No target callbacks, locks, payload capture, shadow tree or Request
instrumentation was added. Existing legacy commands remain for cutover.

Verification completed and stopped:

- x86_64 debug and release image builds passed without compiler warnings/errors.
  Logs: /tmp/arx-filesystem-debug-build.log and
  /tmp/arx-filesystem-release-build.log.
- Thirteen focused host tests passed: uncached archive paths, exact selection,
  raw/effective kinds and root marker, invalid counts/paths, alternate managers,
  cache/retired/overlay state, symlinks, bind shadowing, unverified callbacks,
  unsupported backends, malformed links/hash dimensions, budgets and parser.
  Harness: /tmp/arx-filesystem-host.py.
- Focused release preprocessing/symbol checks and compilation against matching
  native root/layout headers passed. Harness:
  /tmp/arx-filesystem-release-check.py.
- Final one-CPU QEMU/GDB run passed against the final backend-identity guard:
  /tmp/arx-filesystem-target-nvk9v1fc/gdb.log and serial.log. Verified parsed
  archive lookup/bounds fixtures, pre-root unavailable state, mounted cpio
  contents including uncached test.txt, custom-backend rename/unlink, bind/move
  visibility and explicit incomplete branches. The fixture's own accumulated
  outcome remained successful and retained-entry counters were populated.
  Cache occupancy stayed unchanged across inspection. Reviewed readers to
  confirm they issue no writes, target functions or cursor/cache mutations.
  Harnesses: /tmp/arx-filesystem-run.py and /tmp/arx-filesystem-target.py.
- Earlier four-CPU VFS inspection passed under
  /tmp/arx-filesystem-target-5w44o0wc/. A later four-CPU run under
  /tmp/arx-filesystem-target-hc5lzlsk/ stopped before the VFS fixture because
  poststartkernel/memory/file_shared_io_writeback could not reclaim its fixture
  task. This is recorded as an incidental cleanup failure; its root cause is
  not established. The focused one-CPU retry passed. No baseline or unrelated
  scheduler investigation was added.
- During harness development, an archive directory named '.' needed explicit
  root-marker handling; tests now cover it. Corrected harness breakpoint order
  (boot root publication precedes request tests) and GDB array indexing. Those
  harness failures did not establish kernel defects. Repeated runs were limited
  to these concrete corrections and the final private-layout identity guard.
- Reviewed layer boundaries, read-only/native semantics, release erasure and
  human-readable formatting. Updated GdbCommands.md, Resource, Logic, Dispatcher
  and KernelArch guides and the migration ledger. No diagram changes were needed.
  Ticket references/index and whitespace checks passed.

Existing generated-bytecode changes were preserved. Verification harnesses stay
under /tmp; no deleted test tooling was restored. No additional runtime testing
is required. All acceptance criteria are met. Callback-only backend enumeration,
ambiguous stored names and interrupted/bounded state remain documented explicit
limitations, not claims of a complete global catalogue.

Status remains in progress pending owner authorization to close and move the
completed ticket to tickets/closed/.

### 2026-10-20 — Owner requested cleanup failure fix

The owner explicitly instructed “fix” after requiring no failures. The previous
ready-for-review assessment is superseded until the recorded four-CPU
poststartkernel/memory/file_shared_io_writeback task-reclamation failure is
resolved. Continuing under that explicit instruction with the diagnosing-bugs
skill, using a focused failing-path breakpoint to capture TaskManager's refusal
state. No scheduler policy change or broad baseline investigation is assumed.

### 2026-10-20 — Cleanup investigation stopped at owner direction

After initially requesting a fix, the owner instructed: “if nots reproducing
thats enough.” Stopped further cleanup investigation under that updated direction.
No cleanup, TaskManager, Scheduler or TestFixture code was changed, and no
regression test was added for an unconfirmed cause.

The focused four-CPU memory-fixture path passed once, then eight bounded repeat
attempts. Restoring the original filesystem debugger-stop sequence also passed
six attempts, followed by six bounded concurrent reproductions using passive
abort-state capture. The historical failure did not recur. Relevant logs are
under /tmp/arx-fixture-cleanup-*, including dqh54n_2, gur8ms23, 7rjhyhlt and
gvdr8dwu. Temporary diagnosis scripts are /tmp/arx-fixture-cleanup-run.py,
/tmp/arx-fixture-cleanup-target.py and /tmp/arx-fixture-original-target.py.

Source inspection found a possible overlap between scheduler dispatch claims
and synchronous fixture reclamation, but it was not confirmed as the historical
failure's cause. No fix is claimed. The earlier failure remains recorded above;
passing reruns do not erase that observation or establish a root cause.

The owner withdrew further failure-fix work when reproduction was unsuccessful.
The VFS/initramfs implementation and its completed verification remain ready for
review. No required work remains under the owner's updated scope. Status stays
in progress until explicit closure authorization.

### 2026-10-20 — Completion

The owner instructed “close” after the proposed move to tickets/closed/.
Closed and moved the completed VFS/initramfs ticket. Archive and visible-namespace
views, owner statistics, native contracts and documentation are complete, with
focused checks passed. The historical cleanup failure and owner-directed stop
of its unsuccessful reproduction remain recorded; no cleanup fix is claimed.
