#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Public Platform records contain no interpreter objects. Device identities are
// immutable, borrowed for the kernel lifetime. Zero is never a device identity.
typedef uint64_t acpi_device_id_t;

typedef enum acpi_status
{
    ACPI_SUCCESS,
    ACPI_NOT_READY,
    ACPI_UNSUPPORTED,
    ACPI_INVALID_ARGUMENT,
    ACPI_NOT_FOUND,
    ACPI_NO_MEMORY,
    ACPI_FIRMWARE_ERROR,
    ACPI_INCOMPLETE,
    ACPI_UNSAFE_CONTEXT,
    ACPI_TIMEOUT,
    ACPI_NEEDS_CONFIGURATION
} acpi_status_t;

typedef enum acpi_stage
{
    ACPI_STAGE_TABLES,
    ACPI_STAGE_HOST,
    ACPI_STAGE_SUBSYSTEM,
    ACPI_STAGE_NAMESPACE,
    ACPI_STAGE_DEVICES,
    ACPI_STAGE_EVENTS,
    ACPI_STAGE_READY
} acpi_stage_t;

typedef struct acpi_runtime_state
{
    acpi_status_t status;
    acpi_stage_t  stage;
    uint32_t      firmware_error;
    size_t        device_count;
    bool          incomplete;
} acpi_runtime_state_t;

typedef struct acpi_device
{
    acpi_device_id_t id;
    acpi_device_id_t parent;
    const char*      path;
    const char*      hardware_id;
    const char*      unique_id;
    const char**     compatible_ids;
    size_t           compatible_id_count;
    uint64_t         address;
    uint32_t         initial_status;
    uint32_t         firmware_error;
    bool             has_address;
    bool             incomplete;
    bool             pci_root;
    uint16_t         pci_segment;
    uint8_t          pci_bus;
    bool             has_pci_address;
    uint8_t          pci_device;
    uint8_t          pci_function;
} acpi_device_t;

typedef enum acpi_resource_kind
{
    ARX_ACPI_RESOURCE_UNKNOWN,
    ARX_ACPI_RESOURCE_IRQ,
    ARX_ACPI_RESOURCE_DMA,
    ARX_ACPI_RESOURCE_IO,
    ARX_ACPI_RESOURCE_MEMORY,
    ARX_ACPI_RESOURCE_ADDRESS,
    ARX_ACPI_RESOURCE_DEPENDENT_START,
    ARX_ACPI_RESOURCE_DEPENDENT_END,
    ARX_ACPI_RESOURCE_END
} acpi_resource_kind_t;

typedef struct acpi_resource
{
    acpi_resource_kind_t kind;
    // Original AML descriptor, including its header, within template_bytes.
    size_t              offset;
    size_t              size;
    uint64_t            minimum;
    uint64_t            maximum;
    uint64_t            alignment;
    uint64_t            granularity;
    uint64_t            translation;
    uint64_t            length;
    uint32_t            interrupts[255];
    uint8_t             interrupt_count;
    bool                level_triggered;
    bool                active_low;
    bool                shared;
    bool                understood;
} acpi_resource_t;

typedef struct acpi_resources
{
    // Pass a zeroed snapshot; release before reuse. Raw descriptors
    // preserve all flags, sources and alternatives, including unknown types.
    // Release owned storage with platform_acpi_free_resources.
    uint8_t*         template_bytes;
    size_t           template_size;
    acpi_resource_t* entries;
    size_t           count;
    bool             has_unknown;
    uint32_t         firmware_error;
} acpi_resources_t;

typedef enum acpi_event_kind
{
    ACPI_EVENT_NOTIFY,
    ACPI_EVENT_POWER_BUTTON,
    ACPI_EVENT_SLEEP_BUTTON,
    ACPI_EVENT_FIRMWARE_FATAL
} acpi_event_kind_t;

typedef struct acpi_event
{
    acpi_event_kind_t kind;
    acpi_device_id_t  device;
    uint64_t          value;
    // Unknown/unpublished namespace paths remain identifiable in notifications.
    char              path[256];
    bool              path_truncated;
} acpi_event_t;

typedef struct acpi_pci_route
{
    uint32_t gsi;
    bool     level_triggered;
    bool     active_low;
    bool     shared;
    acpi_device_id_t link_device;
    uint32_t resource_index;
} acpi_pci_route_t;

#ifdef __cplusplus
extern "C"
{
#endif
    // Initialize once on BSP before scheduling. Failure retains table access.
    acpi_runtime_state_t platform_acpi_initialize(void);
    acpi_runtime_state_t platform_acpi_state(void);
    const acpi_device_t* platform_acpi_device(size_t index);
    acpi_status_t platform_acpi_refresh(acpi_device_id_t device, uint32_t* flags, uint32_t* firmware_error);
    acpi_status_t platform_acpi_resources(acpi_device_id_t device, bool possible, acpi_resources_t* resources);
    void platform_acpi_free_resources(acpi_resources_t* resources);
    acpi_status_t platform_acpi_route_pci(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint8_t pin, acpi_pci_route_t* route);
    // Interrupt allocation policy chooses gsi from the link's _PRS. Platform
    // validates it, preserves the _CRS descriptor layout and executes _SRS.
    acpi_status_t platform_acpi_configure_link(acpi_device_id_t link, uint32_t resource_index, uint32_t gsi);
    acpi_status_t platform_acpi_next_event(acpi_event_t* event, uint64_t* lost, uint64_t deadline);
    acpi_status_t platform_acpi_reset(void);
    acpi_status_t platform_acpi_power_off(void);

    // Publish lifetime workers before normal scheduling starts. No unload.
    bool platform_acpi_start_workers(void);
    void platform_acpi_interrupt_enter(void);
    void platform_acpi_interrupt_leave(void);

    // Pure bounded parser, also used by host-independent resource tests.
    acpi_status_t platform_acpi_decode_resources(const uint8_t* bytes, size_t size, acpi_resources_t* resources);
#ifdef __cplusplus
}
#endif
