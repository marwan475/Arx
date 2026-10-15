# x86_64 post-init stall investigation

## Evidence and limits

The reported hardware boot was not accessible from this environment. No KVM
device was available. QEMU TCG with the repository's q35, `-cpu max`, UEFI and
disk image workflow was available, as were GCC/G++, NASM, GDB, image tools and
the AArch64 cross-compilers. No AGENTS.md instructions were present.

An unmodified optimized eight-CPU boot stalled after AP `smp pid-vfs start`
messages. GDB captured:

* CPU 0: `arch_pause -> ProcessManager::AcquireProcess`, RFLAGS `0x2`
  (interrupts disabled), in the timer scheduler's process scan.
* CPU 5: `arch_pause -> ProcessManager::CreateProcess`, waiting while holding
  the process-manager lock.
* CPU 4: `arch_pause -> tlb_shootdown -> arch_map_range`, RFLAGS `0x202`.
* Other APs: `CreateProcess` or `vmalloc` lock waits, RFLAGS `0x202`.

The source establishes the cycle: CreateProcess held ManagerLock while waiting
for the shared address-space operation_lock; vmalloc holds operation_lock across
mapping and synchronous shootdown; the timer process scan waits for ManagerLock
with IF cleared and cannot acknowledge the shootdown. This is a confirmed code
defect and an observed QEMU deadlock. It is a candidate explanation for the
hardware stall, not proof of the hardware cause. The last printed syscall name
does not identify the waiting operation.

The original single-CPU optimized and eight-CPU DEBUG=1 boots completed. That
difference is consistent with a timing-dependent lock cycle.

## Changes

* CreateProcess acquires operation_lock **before** ManagerLock. It retains the
  memoryUsers/shared-root checks and balances both guards on rejection paths.
  Timer readers can finish their manager transaction while a creator waits for
  a mapping operation. IPIs remain enabled while ordinary preemption is deferred.
* SMP fixtures defer timer dispatch across manual process/task switching, and
  their helper tasks carry their own preemption guards. ExecuteTask saves and
  restores these guards with each task; no interrupt masking was added.
* SMP cleanup unlinks tasks from process lists before freeing reusable task
  slots. Without this, another CPU could reuse a slot between FreeTask and
  FreeProcess, leaving an old process pointing into the new fixture. After the
  deadlock fix, PID mismatches and a skipped sleep peer were observed. GDB found
  the queued peer process pointing at a task already reset to `allocated=false`.
  Unlinking removed the failure with the original 100 ms sleep test retained.
* Shared-root fixture creation uses bounded retries (1024) because FreeProcess
  temporarily pins that root during teardown. Persistent failures still fail
  the test. The production allocation API's rejection semantics are unchanged.
* BSP waits for all SMP fixtures before starting syscall fixtures. These tests
  temporarily replace process identity and exercise memory ownership; overlapping
  their fixture lifetimes caused legitimate shared-root attachment rejections.
* Completed post-init tasks halt until interrupts instead of busy-spinning.
* BootstrapPostInitTasks was inspected: it publishes per-CPU seed task/process
  state and ready entries before bsp_kmain_exited releases AP dispatch. No change
  to that publication protocol was supported by the evidence.

VFS file/inode/cache locks, task context switching, LAPIC calibration, IPI mailbox
acknowledgements and the scheduler sleep path were inspected. No separate LAPIC
timer calibration or VFS-lock defect was established. No speculative timer-rate,
shootdown-timeout or interrupt-enabling workaround was applied.

## Hardware diagnostics

`BOOT_DIAGNOSTICS=1` adds symbols while retaining the selected optimization level.
Use separate build directories because make does not track compiler flag changes:

```sh
make -j8 DEBUG=1 BOOT_DIAGNOSTICS=1 BUILD_DIR=build/hwdebug BIN_DIR=bin/hwdebug x86_64
make -j8 DEBUG=0 BOOT_DIAGNOSTICS=1 BUILD_DIR=build/bootdiag BIN_DIR=bin/bootdiag x86_64
```

There are per-CPU checkpoints for bootstrap allocation, PID/VFS operations,
scheduler probes, syscall names, and sleep peer setup/parking/wakeup/cleanup.
Long spinlock or LAPIC ICR delivery waits emit one `BOOT WAIT` per acquisition
after 2^24 polling iterations. On x86, a checkpoint unchanged for 1000 LAPIC
timer interrupts also emits a report (roughly ten seconds after calibration).
This is an observation threshold, not a timeout: it never treats an incomplete
shootdown as completed or frees memory before acknowledgement.

Reports contain CPU, last checkpoint, lock address, call site/interrupted PC,
reporter RFLAGS, preemption depth and, for mailbox locks, destination CPU, pending
request type and its last checkpoint. `lock=0` is the timer checkpoint watchdog;
its flags reflect the interrupt handler, not the interrupted task. Checkpoints
are the latest **CPU** activity, not a saved per-task execution trace. Resolve PCs
against the ELF that produced the image, for example:

```sh
addr2line -e bin/bootdiag/kernel-x86_64.elf -f -C 0xffffffff80000000
```

Reporting does not allocate, read the clock, or wait for a logger lock. It writes
directly to serial and attempts the framebuffer lock once. A busy framebuffer is
skipped; simultaneous serial reports can interleave. Missing reports do not
prove progress, particularly if interrupts or the LAPIC have stopped entirely.

## Reproduce and next hardware test

The existing make workflow can run headless with serial capture:

```sh
make -j8 x86_64
timeout 50s make qemu-x86_64 QEMU_SMP=8 \
  QEMU_COMMON='-m 1024 -smp 8 -serial file:/tmp/arx-8.log -monitor none -no-reboot -no-shutdown' \
  QEMU_X86_64_COMMON='-machine q35 -display none -drive if=pflash,format=raw,readonly=on,file=firmware/x86_64/X86_64_UEFI.fd'
```

Use both `QEMU_SMP=1` and `-smp 1` for the single-CPU comparison. Timeout after
the summary is expected: the kernel remains running. Inspect the summary and
error log, not the timeout status alone. The direct QEMU checks used equivalent
arguments with `-drive ...,snapshot=on`.

Next, boot `bin/hwdebug/arx-x86_64.img` on the **affected machine with all eight
CPUs enabled**. Capture serial output if available and photograph any BOOT WAIT
lines. Allow at least 60 seconds. Expected: all eight CPUs finish PID/VFS and
scheduler probes, all 166 implemented request tests pass, SMP has 32 passes and
zero failures, and the error log is empty. Repeat with the optimized diagnostic
image, then repeat that image with one CPU enabled (expected SMP passes: 4).
Keep each matching ELF with its captured output. If it stalls, retain complete
CPU/operation/lock/caller/IPI-target lines, not just the last normal checkpoint.
Hardware success remains unverified until these boots complete.

## Verification in this environment

All four final boot configurations completed the full suite, reported zero
failures and an empty kernel error log:

| Configuration | Implemented request checks | SMP checks |
| --- | --- | --- |
| Optimized, 1 CPU | 166 passed | 4 passed |
| Optimized, 8 CPUs | 166 passed | 32 passed |
| Optimized + BOOT_DIAGNOSTICS, 8 CPUs | 166 passed | 32 passed |
| DEBUG=1 + BOOT_DIAGNOSTICS, 8 CPUs | 166 passed | 32 passed |

The diagnostic path was also tested by setting the kernel operation_lock to 1
at smp_selftests entry in a disposable single-CPU QEMU guest. It reported:

```text
BOOT WAIT cpu=0 op=create-process:address-space-lock lock=FFFFFFFF800F9031 caller=FFFFFFFF8004AB5E flags=246 preempt=2 ipi_target=-1 pending=0 target_op=-
```

A later timer watchdog report identified the same CPU and operation. The call
site resolved against the diagnostic ELF to the CreateProcess lock wait. This
guest was deliberately stuck; it was not counted as a successful boot.

The optimized and diagnostic x86 images built successfully. An AArch64 kernel
cross-build also succeeded; its initial full build emitted the existing
`arch/aarch64/arch_init.c:29` noreturn warning. AArch64 runtime was not tested.
`git diff --check` passed.

Serial logs, the original optimized ELF and final image/ELF checksums are retained
locally under `build/boot-stall-evidence/`. The latest optimized diagnostic boot
is `diagnostic-final-8.log`. Generated images, logs and build products are not
source changes.
