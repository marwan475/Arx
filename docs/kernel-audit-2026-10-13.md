# Kernel audit — x86_64 and common code

Reviewed on October 13, 2026. Base Git commit: `c61334588671149463c67efd5a0286b3a4d25e16`, plus the working-tree changes present during review. aarch64 implementation files were excluded.

This is a source audit, not a successful kernel boot or stress test. I traced boot/SMP initialization, interrupt and syscall assembly, APIC/PCI/ACPI integration, physical/virtual/slab memory, scheduler/task/process lifetimes, ELF loading, VFS user copies, file operations, time handling, and relevant selftests. Vendored uACPI, Flanterm, and printf were reviewed at integration boundaries, not exhaustively audited internally. Unimplemented syscalls returning ENOSYS are not counted as defects.

Files were being edited by another actor during the audit. Scheduler queue locking and wrapped-queue compaction were corrected during the review; those early observations are excluded from the outstanding findings. Sleep scheduling was also under active development and is excluded from the count. References below identify the code observed, and should be checked against later edits.

**19 outstanding findings: 10 P1 and 9 P2.** P1 means serious corruption, isolation failure, hang, or loss of kernel operation under the stated trigger. P2 means a narrower correctness or hardware compatibility problem. These priorities describe impact, not runtime reproduction status.

## Findings

### 1. [P1] User-copy APIs accept kernel addresses and ignore page permissions

Location: [VirtualMemoryManager.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/VirtualMemoryManager.cpp:119), especially `IsUserRangeAccessible`, `CopyFromUser`, and `CopyToUser`.

The validator explicitly accepts the entire kernel canonical range without checking whether it is mapped. The copy then performs a direct memcpy. VFS read/write/stat/getcwd and system/scheduler syscalls still use these APIs; the VFS preliminary check rejects only null and addresses below 0x1000. A userspace caller can therefore request a copy into writable kernel memory, or copy kernel memory into an initramfs file using write. An unmapped kernel pointer can instead fault the kernel.

For low addresses, validation checks only physical translation, not effective U/S and writable permissions. A VFS read can write through a read-only user destination, or access a PROT_NONE supervisor mapping; depending on CR0.WP, this either bypasses permissions or raises an unrecoverable kernel fault. The validate/copy gap also permits mapping changes from another CPU because these legacy helpers do not hold operation_lock or pin the process.

Fix: migrate every syscall to the newer pinned ReadCurrentUserMemory/WriteCurrentUserMemory path, reject the kernel half, validate effective permissions, and preserve mapping lifetime across the copy. Keep test-only kernel-buffer helpers separate from the syscall interface.

### 2. [P1] Building the initial user stack writes outside the backing page

Location: [ProcessManager.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/ProcessManager.cpp:37), `copy_to_process_virtual`.

`vmm_virt_to_phys(currentVirtualAddress)` already returns the physical address including the page offset. The helper converts that address to HHDM, then adds pageOffset again in memcpy. At virtual offset 0xff8, an eight-byte word is written at physical offset 0x1ff0 relative to the page base. This corrupts another physical page and leaves the intended stack data unwritten. argv/envp strings and metadata words commonly use nonzero offsets.

Fix: translate the aligned page base and add the offset once, or copy directly to the translated address without adding it again. A model of the exact address arithmetic reproduced the offset error; no guest execution was available.

### 3. [P1] Allocator and VFS spinlocks allow their owner to be preempted

Locations: [klib.c](/C:/Users/marwa/develop/Arx/klib/klib.c:424), [pmm.c](/C:/Users/marwa/develop/Arx/kernel/platform/memory/pmm.c:396), [vmm.c](/C:/Users/marwa/develop/Arx/kernel/platform/memory/vmm.c:235), and [VirtualFileSystem.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Logic/VirtualFileSystem.cpp:459).

kmalloc/kfree, PMM/VMM locks, logging, and VFS locks use ordinary spinlocks without suppressing local scheduling. The LAPIC timer schedules kernel tasks whenever preemption_depth and syscall active are zero. If kernel task A is preempted while holding heap.lock and the replacement user task enters a syscall that calls kmalloc, that syscall spins on the same CPU. Timer scheduling is suppressed while syscall active is set, so A cannot resume to release the lock. Similarly, an mmap syscall can spin on a PMM/VMM lock held by its preempted kernel predecessor. This applies to ordinary kernel task owners even though syscalls and some manager operations already suppress preemption.

Fix: make the relevant lock wrappers suppress preemption before acquisition and restore it after release. Keep TLB IPIs enabled while waiting for memory locks. Exception logging also needs an emergency path that cannot spin on a lock held by the faulting task.

### 4. [P1] SMP completion barrier can lose arrivals and hang boot

Location: [bootstrap.c](/C:/Users/marwa/develop/Arx/kernel/bootstrap.c:190), also the initialized polling loop at line 151.

`platform.cpus_initialized++` is a non-atomic shared increment. Two cores can read the same value and both store the same incremented value, permanently leaving the count below cpu_count. Its polling loads and the initialized flags are ordinary non-atomic accesses without acquire/release publication. The compiler is not required to continually observe concurrent updates.

Fix: publish readiness with release stores, use acquire loads, and use an atomic fetch-add or a per-CPU arrival bitmap for the final barrier. Test simultaneous arrivals at several SMP sizes.

### 5. [P1] Hardware APIC IDs are used as dense CPU-array indices

Locations: [arch_entry.c](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_entry.c:142), [cpu.c](/C:/Users/marwa/develop/Arx/kernel/platform/cpu/cpu.c:27), and [arch_acpi.c](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_acpi.c:113).

arch_cpu_id returns CPUID's eight-bit initial APIC ID, while cpus_init initializes slots 0..cpu_count-1 and MADT parsing assigns LAPIC records by their order in the table. APIC IDs need not be consecutive or match MADT order. On a two-core topology with IDs 0 and 2, CPU 2 uses a slot with no NUMA/address-space initialization, while the boot barrier waits for slot 1. An ID of 8 or greater indexes beyond the eight-entry CPU array. A reordered MADT can also direct shootdown IPIs to the wrong hardware CPU.

The boot code truncates the first eight Limine entries without ensuring the BSP is in that selection. IOAPIC routing additionally assumes the BSP occupies slot 0.

Fix: assign dense logical indices from the selected Limine CPU list, retain full hardware IDs separately, always retain the BSP, and match MADT records by hardware identity rather than enumeration order. Explicitly reject unsupported x2APIC configurations.

### 6. [P1] Kernel TLB invalidation skips CPUs running user roots

Locations: [arch_paging.c](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_paging.c:143) and `arch_create_user_page_table` at line 882.

User PML4 roots copy the kernel half's pointers and share its lower-level kernel tables. tlb_shootdown only targets CPUs whose root physical address exactly equals the root being modified. A CPU executing under a different user root still uses the shared kernel mapping but receives no invalidation when the kernel root changes it. Kernel vmalloc pages are marked global, so switching CR3 does not necessarily remove their stale translations.

Trigger: CPU B caches a kernel vmalloc mapping under a user root; CPU A unmaps/remaps that mapping through the kernel root. B can keep accessing the old physical page after reuse.

Fix: kernel/shared/global mapping changes need invalidation on every CPU that can use them, independently of the current user root. Also serialize shared upper-half table mutation through a common ownership/locking mechanism.

### 7. [P1] vfree returns physical pages before removing mappings

Location: [klib.c](/C:/Users/marwa/develop/Arx/klib/klib.c:390), also `rollback_vmalloc` at line 245.

The free loop calls pmm_free while the old PTEs and remote TLB entries remain valid; vmm_unmap_range occurs only after the loop. A concurrent PMM allocation can reuse the physical page while it is still accessible through the former virtual address. This violates the mapping lifetime boundary and increases the impact of finding 6.

Fix: retain the backing-allocation metadata, unmap and complete all required shootdowns first, then return physical blocks to PMM. Apply the same ordering to rollback. Coordinate concurrent frees and region metadata lifetime.

### 8. [P1] Successful user exit returns through the syscall return path

Location: [ProcessRequestManager.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Request/ProcessRequestManager.cpp:73).

If KillProcess succeeds, HandleExitRequest immediately returns 0 before reaching the isUserTask check and switch-away loop. Normal user exit and exit_group therefore return via iretq. The process is marked exited and no longer requeued, but its current task keeps executing until a later switch, and can keep executing indefinitely when there is no replacement.

The ELF selftest intercepts its own exit in selftest_syscall_dispatch and switches back to the harness, so that fixture does not validate this production handler.

Fix: separate returnable kernel-test behavior from user exit. User exit must switch away permanently and defer stack, task, and process reclamation until the context is quiescent.

### 9. [P1] Scheduling and teardown do not atomically claim task ownership

Locations: [Scheduler.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Logic/Scheduler.cpp:340), `KillProcess`, and [TaskManager.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/TaskManager.cpp:259).

The newer ScheduleTask checks other CPUs' RunningProcesses and then activates a process without a common lock or atomic state transition. Two CPUs can both pass that check before either publishes ownership. QueueLock does not protect this check/claim interval: entries are removed before it. A running process may also be enqueued on another CPU, which makes the interval reachable without requiring duplicate ready-queue entries.

KillProcess separately checks RunningTasks and then frees tasks; ExecuteTask separately checks allocation and then uses the context. A scheduler racing that check can begin using a task that teardown resets and frees. FreeTask itself does not reject a currently running task; it clears RunningTasks and frees its stack.

Fix: use explicit task states and atomic/locked claims for running, blocked, dying, and reclaimable transitions. Queue membership alone is not ownership. Wait for remote execution to quiesce before freeing a stack or recycling a task slot. These are source-level interleavings, not reproduced guest races.

### 10. [P1] Device ECAM mappings use ordinary RAM cache attributes

Locations: [pci.c](/C:/Users/marwa/develop/Arx/arch/x86_64/pci.c:186) and [acpi.c](/C:/Users/marwa/develop/Arx/kernel/platform/acpi/acpi.c:40).

PCI ECAM is mapped through uacpi_kernel_map, which sets read/write but does not choose device cache attributes. If firmware MTRRs do not independently force UC, configuration accesses can be treated as cacheable RAM, producing stale reads or unsafe device transactions. LAPIC/IOAPIC initialization also skips attribute enforcement when it believes a mapping already exists.

Fix: distinguish firmware-table RAM from device MMIO mapping. Establish a documented PAT/MTRR policy for ECAM/APIC/HPET and prevent conflicting cache-type aliases. Intel's system programming manual describes the memory-type requirements; exact effective type depends on the platform's PAT/MTRR configuration.

### 11. [P2] PCI ECAM addresses are wrong for a nonzero starting bus

Location: [pci.c](/C:/Users/marwa/develop/Arx/arch/x86_64/pci.c:186), with offset calculations at lines 47 and 92.

The code maps the raw MCFG base and uses bus-start_bus as its bus offset. MCFG base addresses correspond to bus 0, including descriptors with a nonzero starting bus. For a region starting at bus 0x40, the first scan accesses base instead of base+0x04000000. Devices can disappear or reads can target the wrong bus.

Fix: map base+(start_bus<<20) before using a relative bus index, with overflow checks. This convention is confirmed in the [Linux kernel PCI ACPI documentation](https://kernel.org/doc/html/v5.12/PCI/acpi-info.html).

### 12. [P2] IOAPIC register-window accesses are not serialized

Location: [ioapic.c](/C:/Users/marwa/develop/Arx/arch/x86_64/ioapic.c:61), `route_irq`, `ioapic_set_vector_mask`, and `ioapic_register_device`.

IOREGSEL and IOWIN form one shared indirect register interface. CPU A can select pin A, CPU B select pin B, and A then read or modify pin B. Concurrent registration also races platform.vector_base, allowing duplicate vector assignment. These APIs are exposed without a caller serialization contract.

Fix: hold one per-controller lock across each entire selection/access or redirection update and serialize vector allocation. Select the real BSP using the logical CPU mapping from finding 5.

### 13. [P2] LAPIC spurious interrupts panic the kernel

Locations: [lapic.c](/C:/Users/marwa/develop/Arx/arch/x86_64/lapic.c:59) and [interrupt_handler.c](/C:/Users/marwa/develop/Arx/arch/x86_64/interrupt_handler.c:156).

The LAPIC spurious vector is configured as 0xff. ISRHANDLER treats it as an ordinary external interrupt, issues EOI, falls through to logging, and panics. A legitimate spurious interrupt is therefore fatal. Intel specifies that this handler should return without EOI because spurious delivery does not set an in-service bit.

Fix: recognize the configured spurious vector before the general EOI path and return. Reference: [Intel SDM, volume 3A, section 12.9](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf).

### 14. [P2] The canonical-address check omits bit 47

Location: [arch_paging.c](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_paging.c:106).

The validator checks that bits 63:48 are all zero or all one, without ensuring they extend bit 47. It accepts 0x0000800000000000 and 0xffff000000000000, both invalid under the four-level paging model used by the walker. The page-table index for the first example aliases the index of a real high-half address, so accepting it for mapping or protection can mutate that real mapping. INVLPG on a noncanonical address does not invalidate the corresponding canonical alias.

Fix: validate the sign extension of bit 47 and reject ranges crossing the canonical hole. The two false acceptances were reproduced with an arithmetic model. Reference: [Intel SDM, volume 3A, sections 4.5.1 and 4.5.4](https://cdrdv2-public.intel.com/835754/253668-sdm-vol-3a.pdf).

### 15. [P2] Mapping allocation failures are hidden from callers

Locations: [arch_paging.c](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_paging.c:670), [klib.c](/C:/Users/marwa/develop/Arx/klib/klib.c:322), and [ElfMapper.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Logic/ElfMapper.cpp:581).

The walker reports allocation failure, but arch_map_page/range discard that result and expose void APIs. vmalloc increments mapped_size and returns a pointer after a failed or partial map. ELF loading records a page as mapped without checking translation and can report executableLoaded=true even when an allocation failure prevented installation of its PTE. Failure paths can leave intermediate tables allocated.

Fix: propagate structured mapping status and roll back partial mappings/tables. Add allocation-failure injection at each page-table level, rather than checking only successful PMM backing allocation. Anonymous-page allocation already does a translation check, but the legacy vmalloc and ELF paths do not.

### 16. [P2] Large bootloader mappings are silently skipped

Location: [arch_paging.c](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_paging.c:369), `x86_64_decode_table_entry` and its callers.

Any PDE/PDPTE with PS set is rejected as if it were absent. arch_virt_to_phys therefore returns 0 for valid 2 MiB/1 GiB mappings. Unmap/protect walks skip them, and map cannot descend into them. Bootloader-provided HHDM mappings may use these page sizes, so the behavior depends on boot layout and hardware memory size.

Fix: translate large leaves with their actual offset masks. Either split a large leaf for subrange mutation or return an explicit unsupported/error status; do not silently treat a present leaf as missing.

### 17. [P2] Initial user stack alignment depends on argc/envc parity

Location: [ProcessManager.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Resource/ProcessManager.cpp:582).

The cursor is aligned to 16 before metadataByteCount is subtracted. When metadataWordCount is odd, the resulting user RSP is 8 modulo 16. For argc=0, envc=0, auxvCount=1, metadata occupies 40 bytes and creates this misalignment. arch_enter_user_mode explicitly panics unless RSP is 16-byte aligned. The existing ELF test uses a combination yielding an even word count and misses it.

Fix: reserve padding while laying out metadata so the final entry RSP, not the intermediate string cursor, is aligned. The parity error was reproduced with an arithmetic model.

### 18. [P2] Positioned I/O temporarily changes a shared file offset

Location: [VfsRequestManager.cpp](/C:/Users/marwa/develop/Arx/kernel/layers/Request/VfsRequestManager.cpp:433), `perform_positioned_user_buffer_io`.

pread/pwrite save file.offset, call Seek, perform ordinary I/O, then restore the offset. Each VFS operation takes and releases ioLock independently. Another task/CPU using a duplicated or retained file can observe the temporary offset, read/write at the wrong location, and then have its legitimate offset update overwritten by the restoration.

Fix: add positioned backend operations that take an explicit offset without changing file.offset, or hold the file lock over the entire transaction using internal helpers that do not reacquire it. Separately, FD lookup/close/table growth need synchronized lookup and a retained file reference: current syscall reads borrow a raw pointer while close can free it.

### 19. [P2] Double-fault handling has no independent emergency stack

Location: [arch_init.c](/C:/Users/marwa/develop/Arx/arch/x86_64/arch_init.c:168).

Every IDT entry uses IST=0. Although IST1 is filled in, it is never selected and initially points at the ordinary initialization stack rather than a dedicated exception stack. When a kernel stack becomes unusable, delivery of #PF/#DF may fail on that same stack, leading to a triple fault and reset instead of diagnostics.

Fix: allocate an independent per-CPU double-fault stack, select its IST slot for vector 8, and consider separate stacks for NMI/machine check. Add guard pages to task stacks and validate controlled overflow in a guest.

## Validation and follow-up

The address-offset and stack-parity arithmetic and canonical validator were reproduced as small JavaScript models of the source. These checks are not native kernel tests. The early wrapped-queue model produced [101,102,101,102] from [101,102,103,104] with head=62 in a 64-slot ring, but the later source copies unread entries into a temporary array and fixes that particular defect.

Native gcc/clang, make, NASM, and QEMU were not available on PATH. WSL reported that it was not installed. No dependencies were installed, no kernel runtime tests were run, and no existing source files were modified by this audit.

Suggested verification order:

1. Exercise real production VFS/syscall dispatch with kernel-half, unmapped, read-only, and PROT_NONE buffers. Use isolated sacrificial pages so a test does not intentionally overwrite live kernel objects.
2. Verify user stack bytes against an expected argv/envp/auxv layout, including offsets near page boundaries and both metadata parities.
3. Stress preemptible kernel allocation and logging with an aggressively short timer interval; separately inject exceptions while a log lock is held.
4. Boot with synthetic sparse/reordered APIC IDs and simultaneous barrier arrivals.
5. Cache a shared kernel VA on a CPU using a user root, then replace it from the kernel root and check that no stale physical frame remains accessible.
6. Race schedule/kill/close against context switching and FD lookup while checking state ownership and stack/file lifetime.
7. Test an MCFG range starting at a nonzero bus, concurrent IOAPIC programming, spurious vector delivery, large HHDM leaves, and page-table allocation failures.

The scheduler/time/task files changed during the review. A late scheduler snapshot included QueueLock, SleepUntil, and a temporary array in RemoveProcessFromQueue. Review findings about task ownership apply to that later check-then-activate implementation, not the earlier missing queue locks. A final validation should use a frozen revision after the ongoing edits complete.
