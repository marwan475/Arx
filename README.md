# Arx
Multiarch 64 bit Kernel in development

Targets:
- x86_64 (currently supported)
- aarch64 (experimental)

## Design

![Arx](docs/imgs/Arx_Arch.png)

Design in order of initialization

Platform
- Architecture specific initiliztion
- Sets up klib
- only part that accesses hardware directly

klib
- contains kernel stdlib
- only access platform for memory allocation functions (kmalloc/vmalloc)

Layer stack
- layers can only access the layer right bellow them using the layers import caps
- lower layers cannot access layers above
- protected by the fact the layers will only have caps struct of layers bellow them

Resource
- Kernel resource manager
- Manages any resources/objects that the kernel owns
- Accesses hardware via platform

Logic
- main logic of kernel
- gets hardware/kernel resources from resource layer
- decides what to do with resources

Request
- interface for kernel request from syscalls or interrupts

Dispatcher
- *not imaged
- how platform will access layer stack

## Kernel Software Stack
- Resource
    - Task manager
        - Owns task objects and task lifecycle
        - Allocation, creation, cleanup
    - Process manager
        - Owns process objects and process lifecycle
        - Tracks process task membership and address space
        - Owns process file descriptor tables
    - Resource filesystem
        - Backend filesystem operations (mount, lookup, read, write)
        - Returns opaque backend node handles

- Logic
    - Scheduler and kernel execution policy
    - ProcessController
        - Process creation, execution, waiting, groups and sessions
        - Process memory mapping, protection and program break policy
        - Implemented in `ProcessLifecycle.cpp` and `ProcessMemoryManager.cpp`
    - Virtual filesystem semantics (paths, dentries, mounts, open files)
    - ElfMapper
        - Reads ELF metadata from VFS files
        - Maps PT_LOAD segments into process address spaces via Resource capabilities
- Request
    - Entry layer for syscalls/interrupt-driven kernel requests
    - Managers currently in tree:
        - ProcessRequestManager
        - SchedulerRequestManager
        - TimeRequestManager
        - MemoryRequestManager
        - VfsRequestManager
        - EventRequestManager
        - SyncRequestManager
        - SignalRequestManager
        - CredentialRequestManager
        - SystemRequestManager
        - SocketRequestManager

    ### Interrupt Scheduling
    - `InterruptRequestManager` handles arch-independent interrupt request numbers (for example schedule request `1`).
    - x86_64 timer interrupts are translated into this request and routed through Dispatcher into Request layer.
    - Schedule requests are tick-gated (`SCHEDULE_TICK_INTERVAL`) to avoid switching every timer tick.
    - On schedule request, current process is re-queued on its CPU ready queue and scheduler runs the next ready process.

## Processes
- Tasks
    - unit of schedulable cpu execution
    - contain stack and task function
    - holds cpu context
- Process
    - stores tasks part of process
    - storess address space

### Filesystem
- Resource owns backend filesystem access (mount, lookup, read, write).
- Logic owns VFS behavior (path resolution, dentries, mounts, open files).
- Resource returns opaque backend handles.
- Logic wraps them into VFS objects (`inode_t`, `dentry_t`, `mount_t`, `file_t`).

Current backend:
- `cpio` initramfs via `ResourceFileSystem`.

Flow:
1. Kernel creates Resource and Logic layers.
2. Logic mounts root filesystem through Resource.
3. Resource provides root backend node.
4. Logic builds root VFS mount/namespace.

### ELF Loading

`ElfMapper` lives in Logic. Call `ReadElf(file, metadata)` first, with
zero-initialized metadata, then either:

- `LoadExecutable(process, file, metadata)` maps the main executable and sets
  its initial heap break.
- `LoadLibrary(process, file, metadata, requestedLoadBias = 0)` maps an `ET_DYN`
  image without changing the heap. Zero selects a free aligned image span;
  a nonzero bias must fit exactly and satisfy segment alignment.

## Third Party
- [Limine](https://github.com/limine-bootloader/limine) - Bootloader/protocol used to load the kernel and provide boot info.
- [printf](https://github.com/mpaland/printf) - Small freestanding printf implementation used for kernel logging output.
- [uACPI](https://github.com/uACPI/uACPI) - ACPI implementation used for table parsing and ACPI support.
- [Flanterm](https://github.com/Mintsuki/Flanterm) - fast and reasonably complete terminal emulator
- [musl](https://musl.libc.org/) - required userspace runtime for dynamic ELF self-tests; its license is bundled with initramfs.

All third-party components retain their original licenses.

## AI Usage
- Build system and scripts are AI generated
- selftest.c is AI generated testing of kernel subsystems
- Ai used for refactoring and codebase managment
- Ai implements syscall handlers using layer import caps
