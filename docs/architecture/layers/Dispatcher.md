# Dispatcher

Dispatcher connects platform and architecture entry points to the kernel's layer
stack. It constructs the layers, exposes their services, and routes syscalls and
common interrupt requests. It also supplies C-callable bridges for code that
needs a kernel service without depending on the C++ manager classes.

`Dispatcher.hpp` declares the class; `Dispatcher.cpp` implements it and the bridges.
KernelArch.md explains the overall design, and x86_64.md describes machine entry
and return. Dispatcher is separate from the global `platform` state.

DispatcherBridge.h is the manager-independent interface used by Resource and
platform for typed operations and opaque route handles. DispatcherRoutes.hpp
defines immutable provider records with private callable fields. Providers keep
their routes and contexts alive until the last task, file reference, or deferred
backing operation has finished; the gateway does not extend those lifetimes.

## 1. Construction and startup

In `kernel.cpp`, `kmain` creates Dispatcher and publishes its address in
`platform.dispacher`. That field's spelling follows the current source.
The constructor creates three factory objects; their managers are created later
by `StartKernel`. In debug builds Dispatcher also embeds a fixed-capacity
DebugManager registry for borrowed, typed statistics records and rich-view roots.
Controlled C/C++ foundation records publish at staged startup points; production
provider rollout is separate. GdbCommands.md documents discovery and authoring.
Release builds exclude the registry, its initialization and all tracking work.

`StartKernel` performs these steps:

1. Publish platform-owned debug providers, create Resource, register its DMA
   provider in debug builds, and run Resource self-tests.
2. Create Logic with Resource capabilities and run its self-tests.
3. Create Request with both Resource and Logic capabilities.

This order makes lower-layer services available before their consumers are
constructed. `StartKernel` then returns to `kmain`, which mounts the root
filesystem, queues post-init work, and activates scheduling.

Construction happens during bootstrap. Request tests run later in scheduled
post-init work, rather than immediately after Request construction.

## 2. Ownership and capabilities

Dispatcher owns the three factory pointers. Each factory owns its layer's manager
objects and exported capability structure. The getters return borrowed references;
callers do not delete them or the managers they contain.
`GetResourceLayerCaps`, `GetLogicLayerCaps`, and `GetRequestLayerCaps` return null
until the corresponding factory has created its capabilities.

| Structure | Purpose |
| --- | --- |
| `ResourceLayerCaps` | References to resource managers and the filesystem backend interface. |
| `LogicLayerCaps` | References to the managers that implement kernel behavior and policy. |
| `RequestLayerCaps` | References to syscall and interrupt request handlers. |
| `arch_syscall_frame_t` | Architecture-supplied syscall number, arguments, and saved user return state. |
| `user_memory_access_t` | Process manager, pinned process, and address space for a user-memory operation. |

The capability structures are declared in `ResourceLayerFactory.hpp`,
`LogicLayerFactory.hpp`, and `RequestLayerFactory.hpp`. They contain pointers;
they do not enforce memory isolation. Request may use either lower layer, and
Logic may use Resource. Managers can also collaborate within their own layer.

Normal startup keeps Dispatcher and its factories for the kernel lifetime.
Its destructor invalidates debug registrations before deleting the Resource,
Logic, and Request factories in that order.
The code does not provide a coordinated running-kernel shutdown protocol, and
factory teardown should not be treated as one.

## 3. Syscall routing

Architecture code calls `dispatcher_dispatch_syscall` with a valid saved frame.
The bridge obtains the published Dispatcher, accounts entry into kernel mode,
and calls `DispatchSyscall`.

`DispatchSyscall` switches on the syscall number and passes the frame to the
selected Request manager. Managers decode arguments and invoke lower-layer
services; Dispatcher forwards their return values.

| Request manager | Example request group |
| --- | --- |
| ProcessRequestManager | Process creation, execution, exit, waiting, and process IDs. |
| SchedulerRequestManager | Policy, priority, CPU affinity, and yielding. |
| TimeRequestManager | Clocks, sleeps, timers, and alarms. |
| MemoryRequestManager | Mappings, protection, memory advice, and program break. |
| VfsRequestManager | Files, paths, descriptors, and related filesystem operations. |
| EventRequestManager | Readiness waits and notification interfaces. |
| SyncRequestManager | Futex operations and robust lists. |
| SignalRequestManager | Signal setup, sending, masks, waiting, and return. |
| CredentialRequestManager | User/group identity and supplementary groups. |
| SystemRequestManager | System information, limits, controls, and random data. |
| SocketRequestManager | Socket creation, connections, messages, and options. |

After the handler returns, the C bridge checks whether an exited process still
has a running user task. If so, it asks Scheduler to exit that task. Otherwise,
the result returns to architecture code. On x86_64, architecture return hooks
then service pending scheduling, consider signal delivery, and account user mode.
Dispatcher itself does not restore registers or execute the user-return instruction.

## 4. Interrupt requests and runtime flow

Architecture code translates a hardware event into a common request number before
calling `dispatcher_handle_interrupt_request`. The number is a kernel request,
not the original hardware vector.

`DispatchInterruptRequest` forwards it to InterruptRequestManager. Currently,
request `1` means scheduling. That manager checks its per-CPU tick interval, wakes
sleepers, and uses Scheduler and ProcessManager to run ready work when scheduling
is active. Interrupt acknowledgment, machine registers, and safe preemption points
remain the responsibility of the architecture entry code.

These are the two main request routes. The service bridges below also support
kernel-service Request entries and necessary Resource-to-Logic operations.

## 5. C bridges for kernel services

The bridges in `Dispatcher.cpp` connect C and architecture code to Request entry
methods and mediate necessary Resource-to-Logic work. They are part of the actual
boundary wiring, beyond the syscall switch.

| Bridge | Responsibility |
| --- | --- |
| `dispatcher_signal_return` | Routes to SignalRequestManager for accounting, rescheduling, and pending signal delivery. |
| `dispatcher_account_mode` | Routes current user/kernel accounting through SchedulerRequestManager. |
| `dispatcher_account_scheduling` | Marks scheduling overhead and task-execution accounting boundaries through Scheduler. |
| `dispatcher_cleanup_task` | Invokes an opaque priority/retirement route while TaskManager preserves the task claim. |
| `dispatcher_vfs_file_operations` | Returns the stable opaque VFS retain/release route, without exposing its callbacks. |
| `dispatcher_retain_file`, `dispatcher_release_file` | Retain or release file references through their provider routes, including descriptor-copy rollback. |
| `dispatcher_account_minor_fault` | Routes to MemoryRequestManager to charge the active process when its address space matches. |
| `dispatcher_service_pending_reschedule` | Routes platform preemption requests through SchedulerRequestManager to Scheduler. |
| `begin_user_memory`, `end_user_memory` | Route to MemoryRequestManager to establish and release Klib's pinned process-memory context. |
| `dispatcher_write_backing`, `dispatcher_release_backing_context` | Route platform backing I/O and provider-context release through MemoryRequestManager. |
| `dispatcher_backing_can_write` | Inspect a route's write capability without exposing or invoking its provider. |

Signal return receives a mutable saved frame and the syscall result. Dispatcher
routes to SignalRequestManager, which identifies restartable operations;
SignalManager and the architecture signal hooks decide delivery and frame changes. When the Logic
capabilities or SignalManager are unavailable, the bridge returns the original
result.

The accounting and rescheduling bridges are used by architecture entry code,
`cpu.c`, and TaskManager. VMM calls the minor-fault bridge when resolving
copy-on-write faults. These routes let lower-level mechanisms reach the stack
while keeping process policy out of those mechanisms. Necessary Resource
operations go through Dispatcher directly to their Logic providers. Platform and
Klib entry goes through Dispatcher into typed Request handlers. The kernel-service
handlers are stateless and retain early Resource-test availability without
requiring construction of the full Request factory output.

## 6. User-memory access and execution constraints

`begin_user_memory` delegates to MemoryRequestManager, which disables
preemption, pins the current process through
ProcessManager, and acquires its address-space operation lock. It fills the
`user_memory_access_t` declared in `klib.h`. Klib can then validate mappings and
copy data while process teardown and mapping changes are excluded.

Every successful begin must be paired with `end_user_memory`. End releases the
lock and process pin, clears the context, and restores the preemption nesting
level. A failed begin returns false and undoes its own preemption guard. The
context must not be retained across a sleep or context switch.

Dispatcher has no global routing lock. The current boot path constructs and
publishes the stack once; concurrent requests rely on manager synchronization
and the architecture/platform interrupt and preemption rules. Saved syscall
frames belong to the caller's active entry context and must remain valid for
the call. Capability getters do not extend object lifetimes.

## 7. Failure behavior and inspection

| Condition | Current behavior |
| --- | --- |
| No published Dispatcher for a syscall | Returns `-ENOSYS` (`-38`). |
| Null frame passed to `DispatchSyscall` | Returns `-EINVAL` (`-22`). |
| Request capabilities unavailable or syscall unknown | Returns `-ENOSYS`. |
| Interrupt bridge has no Dispatcher or handler | Returns without dispatching. |
| InterruptRequestManager receives an unknown request | Ignores it. |
| User-memory context cannot be acquired | Begin returns false; callers handle the failed copy. |

The syscall switch assumes its selected managers were constructed successfully;
it does not check every manager pointer. `StartKernel` returns no construction
status. Calls should therefore use the completed boot-time stack, not partially
created capabilities.

Start with `Dispatcher.cpp` to find routing, then the selected Request manager
and its factory imports. `syscalltests.cpp` exercises the C syscall bridge and
returned errors; `SelfTestManager.cpp` coordinates the construction-time and later
test phases. `debug-kernel.sh` and `gdb_extention.py` provide the existing debugger
workflow and inspection commands.
