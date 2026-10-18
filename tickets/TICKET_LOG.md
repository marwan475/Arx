# Ticket log

Current ticket inventory. Each section is sorted by priority (P0–P3), then
filename. Ticket files contain the authoritative details and work history.

## Feature

| Priority | Ticket | Type | Status |
| --- | --- | --- | --- |
| P1 | [Add initramfs and complete supported VFS namespace debug views](feature/add_debug_filesystem_views.md) | feature | open |
| P1 | [Add physical allocation, NUMA, heap and DMA debug views](feature/add_debug_physical_allocation_views.md) | feature | open |
| P1 | [Add platform, CPU, IPI and PCI framework views](feature/add_debug_platform_cpu_pci_views.md) | feature | open |
| P1 | [Track the process tree, task ownership and shared open resources](feature/add_debug_process_resource_views.md) | feature | open |
| P1 | [Add scheduling, blocked-task, futex and timer debug views](feature/add_debug_scheduling_wait_views.md) | feature | open |
| P1 | [Add virtual-memory ownership, backing and x86_64 translation debug views](feature/add_debug_virtual_memory_views.md) | feature | open |
| P1 | [Build the debug-only kernel and GDB framework foundations](feature/build_debug_framework_foundation.md) | feature | open |
| P1 | [Cut over to the debug framework and document the replacement commands](feature/migrate_debug_commands_and_document_framework.md) | feature | open |
| P2 | [Add pipe, memory-file and UNIX socket debug views](feature/add_debug_ipc_socket_views.md) | feature | open |
| P2 | [Add self-test progress and recorded boot-error debug views](feature/add_debug_selftest_error_views.md) | feature | open |
| P2 | [Add signal, epoll and event-notification debug views](feature/add_debug_signal_event_views.md) | feature | open |
| P2 | [Create an agent skill for adding debug statistics and rich views](feature/create_debug_framework_authoring_skill.md) | feature | open |
| P2 | [Identify Arx agent skills and create implementation tickets](feature/plan_kernel_agent_skill_tickets.md) | feature | open |

## Debug

| Priority | Ticket | Type | Status |
| --- | --- | --- | --- |
| P2 | [Resolve legacy GDB symbol and manager discovery defects during cutover](debug/resolve_legacy_gdb_discovery_defects.md) | debug | open |

## In progress

None.

## Closed

| Priority | Ticket | Type | Status |
| --- | --- | --- | --- |
| P1 | [Document overall kernel architecture and responsibilities](closed/document_project_structure.md) | feature | closed |
| P1 | [Remove Resource's dependency on Logic-owned signal definitions](closed/fix_resource_signal_type_dependency.md) | debug | closed |
| P1 | [Route necessary upward calls through Dispatcher](closed/fix_resource_upward_runtime_calls.md) | debug | closed |
| P2 | [Document architecture-specific code](closed/document_architecture_layer.md) | feature | closed |
| P2 | [Document Logic layer](closed/document_logic_layer.md) | feature | closed |
| P2 | [Document platform services](closed/document_platform_layer.md) | feature | closed |
| P2 | [Document Request layer](closed/document_request_layer.md) | feature | closed |
| P2 | [Document Resource layer](closed/document_resource_layer.md) | feature | closed |
| P2 | [Document self-test infrastructure, suites and execution](closed/document_self_tests.md) | feature | closed |
| P2 | [Expand compressed self-test lambdas into readable blocks](closed/expand_compressed_selftest_lambdas.md) | debug | closed |
| P2 | [Expand unbraced first-party control flow](closed/expand_unbraced_first_party_control_flow.md) | debug | closed |
| P2 | [Restore blank-line separation in dense kernel functions](closed/improve_dense_kernel_code_spacing.md) | debug | closed |
| P2 | [Design the Arx debug framework and create implementation tickets](closed/plan_gdb_extension_tickets.md) | feature | closed |
