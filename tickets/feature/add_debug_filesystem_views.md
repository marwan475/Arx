# Add initramfs and complete supported VFS namespace debug views

- Type: feature
- Priority: P1
- Status: open
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
   VFS/initramfs coverage entries for [cutover](migrate_debug_commands_and_document_framework.md).

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

- [ ] Maintained stats and typed roots cover VFS/initramfs/resource backend state without release work/storage.
- [ ] Archive listing/selection/hierarchy preserves old information and adds bounded ownership/mode detail.
- [ ] Cache and mount views preserve useful old output and explicitly distinguish cached state, overlays, retired/unlinked entries and readable counts.
- [ ] Supported visible namespace contents are reconstructed from backend plus runtime/mount state; incomplete/unsupported branches never claim completeness.
- [ ] Invalid paths, count overflow, hash buckets, parent/mount cycles, missing providers and unreadable strings terminate with useful status without target calls.
- [ ] Native-reader path/visibility rules, shared identities, guide and coverage ledger are updated; dump mechanics remain absent.

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
