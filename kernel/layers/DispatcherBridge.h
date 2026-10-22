#ifndef DISPATCHER_BRIDGE_H
#define DISPATCHER_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

struct task_t;
typedef struct dispatcher_task_cleanup       dispatcher_task_cleanup_t;
typedef struct dispatcher_file_operations    dispatcher_file_operations_t;
typedef struct dispatcher_backing_operations dispatcher_backing_operations_t;

#ifdef __cplusplus
extern "C"
{
#endif
    // Platform ACPI requires task identity, blocking and BSP workers. Dispatcher
    // routes these incoming kernel services through AcpiRequestManager; Platform
    // never imports Resource/Logic manager classes. No userspace interface.
    uint64_t dispatcher_acpi_thread_id(void);
    bool dispatcher_acpi_can_wait(void);
    bool dispatcher_acpi_wait(const uint64_t* sequence, uint64_t expected, uint64_t deadline);
    void dispatcher_acpi_wake(const uint64_t* sequence);
    bool dispatcher_acpi_create_worker(void (*entry)(void*), void* argument);
    // Providers own these immutable routes and must outlive their last user.
    void dispatcher_cleanup_task(const dispatcher_task_cleanup_t* route, struct task_t* task);

    const dispatcher_file_operations_t* dispatcher_vfs_file_operations(void);
    bool                                dispatcher_retain_file(const dispatcher_file_operations_t* route, void* file);
    void                                dispatcher_release_file(const dispatcher_file_operations_t* route, void* file);

    // Platform backing I/O enters Request through these Dispatcher bridges.
    bool    dispatcher_backing_can_write(const dispatcher_backing_operations_t* route);
    int64_t dispatcher_write_backing(const dispatcher_backing_operations_t* route, void* context, uint64_t offset, const void* bytes, uint64_t size);
    void    dispatcher_release_backing_context(const dispatcher_backing_operations_t* route, void* context);
#ifdef __cplusplus
}
#endif

#endif
