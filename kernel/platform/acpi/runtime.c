#include <acpi/acpi.h>
#include <acpi/host.h>
#include <acpi/runtime.h>
#include <platform.h>
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/resources.h>
#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#define ACPI_DEVICE_CAPACITY 512
#define ACPI_EVENT_CAPACITY 128
#define ACPI_PCI_DEPTH 32

static acpi_runtime_state_t runtime = { .status = ACPI_NOT_READY, .stage = ACPI_STAGE_TABLES };
static bool attempted;
static acpi_device_t devices[ACPI_DEVICE_CAPACITY];
static uacpi_namespace_node* nodes[ACPI_DEVICE_CAPACITY];
static acpi_event_t events[ACPI_EVENT_CAPACITY];
static size_t event_head;
static size_t event_count;
static uint64_t event_lost;
static uint64_t event_sequence;
static spinlock_t event_lock;
static uacpi_handle operation_lock;

extern void* uacpi_early_table_buffer;

static acpi_device_id_t node_identity(uacpi_namespace_node* node)
{
    for (size_t i = 0; i < runtime.device_count; i++)
    {
        if (nodes[i] == node)
        {
            return i + 1;
        }
    }

    return 0;
}

static uacpi_namespace_node* device_node(acpi_device_id_t identity)
{
    if (identity == 0 || identity > runtime.device_count)
    {
        return NULL;
    }

    return nodes[identity - 1];
}

static bool device_has_id(const acpi_device_t* device, const char* hardware_id)
{
    if (device->hardware_id != NULL && strcmp(device->hardware_id, hardware_id) == 0)
    {
        return true;
    }

    for (size_t i = 0; i < device->compatible_id_count; i++)
    {
        if (device->compatible_ids[i] != NULL && strcmp(device->compatible_ids[i], hardware_id) == 0)
        {
            return true;
        }
    }

    return false;
}

static void classify_notification(acpi_event_t* event)
{
    if (event->kind != ACPI_EVENT_NOTIFY)
    {
        return;
    }

    // Notify may run during _INI before inventory publication. Resolve its
    // captured path afterward, without evaluating AML inside the callback.
    if (event->device == 0 && !event->path_truncated && event->path[0] != '\0')
    {
        for (size_t i = 0; i < runtime.device_count; i++)
        {
            if (devices[i].path != NULL && strcmp(devices[i].path, event->path) == 0)
            {
                event->device = devices[i].id;
                break;
            }
        }
    }

    if (event->value != 0x80 || event->device == 0 || event->device > runtime.device_count)
    {
        return;
    }

    const acpi_device_t* device = &devices[event->device - 1];

    if (device_has_id(device, "PNP0C0C"))
    {
        event->kind = ACPI_EVENT_POWER_BUTTON;
    }
    else if (device_has_id(device, "PNP0C0E"))
    {
        event->kind = ACPI_EVENT_SLEEP_BUTTON;
    }
}

void acpi_host_publish_event(const acpi_event_t* event)
{
    uint64_t state = arch_save_and_disable_interrupts();
    spinlock_acquire(&event_lock);

    if (event_count == ACPI_EVENT_CAPACITY)
    {
        if (event_lost != UINT64_MAX)
        {
            event_lost++;
        }
    }
    else
    {
        events[(event_head + event_count) % ACPI_EVENT_CAPACITY] = *event;
        event_count++;
    }

    spinlock_release(&event_lock);
    arch_restore_interrupts(state);
    acpi_host_wake(&event_sequence);
}

static uacpi_status notify_device(uacpi_handle context, uacpi_namespace_node* node, uacpi_u64 value)
{
    (void) context;
    acpi_event_t event = { .kind = ACPI_EVENT_NOTIFY, .device = node_identity(node), .value = value };
    const char* path = uacpi_namespace_node_generate_absolute_path(node);

    if (path != NULL)
    {
        size_t length = strlen(path);
        event.path_truncated = length >= sizeof(event.path);

        if (length >= sizeof(event.path))
        {
            length = sizeof(event.path) - 1;
        }

        memcpy(event.path, path, length);
        uacpi_free_absolute_path(path);
    }
    else
    {
        event.path_truncated = true;
    }

    acpi_host_publish_event(&event);
    return UACPI_STATUS_OK;
}

static uacpi_interrupt_ret fixed_button(uacpi_handle context)
{
    acpi_event_t event = { .kind = (acpi_event_kind_t) (uintptr_t) context };
    acpi_host_publish_event(&event);
    return UACPI_INTERRUPT_HANDLED;
}

static void remember_error(acpi_device_t* device, uacpi_status status)
{
    if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND)
    {
        if (device->firmware_error == 0)
        {
            device->firmware_error = status;
        }

        device->incomplete = true;
        runtime.incomplete = true;
    }
}

static void copy_id(acpi_device_t* device, const char* value, const char** output)
{
    if (value == NULL)
    {
        return;
    }

    *output = kstrdup(value);

    if (*output == NULL)
    {
        remember_error(device, UACPI_STATUS_OUT_OF_MEMORY);
    }
}

static uacpi_iteration_decision discover_device(void* context, uacpi_namespace_node* node, uacpi_u32 depth)
{
    (void) context;
    (void) depth;

    if (runtime.device_count == ACPI_DEVICE_CAPACITY)
    {
        runtime.incomplete = true;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    size_t index = runtime.device_count++;
    acpi_device_t* device = &devices[index];
    device->id = index + 1;
    nodes[index] = node;
    uacpi_namespace_node* ancestor = uacpi_namespace_node_parent(node);

    while (ancestor != NULL && device->parent == 0)
    {
        device->parent = node_identity(ancestor);
        ancestor = uacpi_namespace_node_parent(ancestor);
    }

    const char* path = uacpi_namespace_node_generate_absolute_path(node);
    device->path = path;

    if (path == NULL)
    {
        remember_error(device, UACPI_STATUS_OUT_OF_MEMORY);
    }

    uacpi_status status = uacpi_eval_sta(node, &device->initial_status);
    remember_error(device, status);

    uacpi_namespace_node_info* information = NULL;
    status = uacpi_get_namespace_node_info(node, &information);
    remember_error(device, status);

    if (status == UACPI_STATUS_OK)
    {
        device->has_address = (information->flags & UACPI_NS_NODE_INFO_HAS_ADR) != 0;
        device->address = information->adr;

        if (information->flags & UACPI_NS_NODE_INFO_HAS_HID)
        {
            copy_id(device, information->hid.value, &device->hardware_id);
        }

        if (information->flags & UACPI_NS_NODE_INFO_HAS_UID)
        {
            copy_id(device, information->uid.value, &device->unique_id);
        }

        if ((information->flags & UACPI_NS_NODE_INFO_HAS_CID) && information->cid.num_ids != 0)
        {
            device->compatible_ids = uacpi_kernel_alloc((size_t) information->cid.num_ids * sizeof(char*));

            if (device->compatible_ids == NULL)
            {
                remember_error(device, UACPI_STATUS_OUT_OF_MEMORY);
            }
            else
            {
                memset(device->compatible_ids, 0, (size_t) information->cid.num_ids * sizeof(char*));
                device->compatible_id_count = information->cid.num_ids;

                for (size_t i = 0; i < information->cid.num_ids; i++)
                {
                    copy_id(device, information->cid.ids[i].value, &device->compatible_ids[i]);
                }
            }
        }

        uacpi_free_namespace_node_info(information);
    }

    const char* root_ids[] = { "PNP0A03", "PNP0A08", NULL };
    device->pci_root = uacpi_device_matches_pnp_id(node, root_ids);

    if (device->pci_root)
    {
        uint64_t segment = 0;
        uint64_t bus = 0;
        status = uacpi_eval_simple_integer(node, "_SEG", &segment);
        remember_error(device, status);

        if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND)
        {
            device->pci_root = false;
        }

        status = uacpi_eval_simple_integer(node, "_BBN", &bus);
        remember_error(device, status);

        if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND)
        {
            device->pci_root = false;
        }

        if (status == UACPI_STATUS_NOT_FOUND)
        {
            uacpi_resources* resources = NULL;
            status = uacpi_get_current_resources(node, &resources);
            remember_error(device, status);

            if (status == UACPI_STATUS_OK)
            {
                for (uacpi_resource* resource = resources->entries; resource->type != UACPI_RESOURCE_TYPE_END_TAG; resource = UACPI_NEXT_RESOURCE(resource))
                {
                    if (resource->type == UACPI_RESOURCE_TYPE_ADDRESS16 && resource->address16.common.type == UACPI_RANGE_BUS)
                    {
                        bus = resource->address16.minimum;
                        break;
                    }

                    if (resource->type == UACPI_RESOURCE_TYPE_ADDRESS32 && resource->address32.common.type == UACPI_RANGE_BUS)
                    {
                        bus = resource->address32.minimum;
                        break;
                    }

                    if (resource->type == UACPI_RESOURCE_TYPE_ADDRESS64 && resource->address64.common.type == UACPI_RANGE_BUS)
                    {
                        bus = resource->address64.minimum;
                        break;
                    }
                }

                uacpi_free_resources(resources);
            }
        }

        if (segment > UINT16_MAX || bus > UINT8_MAX)
        {
            remember_error(device, UACPI_STATUS_AML_OUT_OF_BOUNDS_INDEX);
            device->pci_root = false;
        }

        device->pci_segment = segment;
        device->pci_bus = bus;
    }

    if (!device->pci_root && device->has_address && device->parent != 0 && (device->address >> 16) < 32 && (device->address & 0xffff) < 8)
    {
        acpi_device_t* parent = &devices[device->parent - 1];
        uint32_t buses = 0;
        uint32_t class_revision = 0;
        bool pci_parent = parent->pci_root && uacpi_namespace_node_parent(node) == device_node(parent->id);
        uint8_t bus = parent->pci_bus;

        if (parent->has_pci_address && uacpi_namespace_node_parent(node) == device_node(parent->id) &&
            arch_acpi_pci_access(parent->pci_segment, parent->pci_bus, parent->pci_device, parent->pci_function, 8, 4, false, &class_revision) &&
            (class_revision >> 16) == 0x0604 && arch_acpi_pci_access(parent->pci_segment, parent->pci_bus, parent->pci_device, parent->pci_function, 0x18, 4, false, &buses))
        {
            pci_parent = true;
            bus = buses >> 8;
        }

        if (pci_parent)
        {
            device->has_pci_address = true;
            device->pci_segment = parent->pci_segment;
            device->pci_bus = bus;
            device->pci_device = device->address >> 16;
            device->pci_function = device->address;
        }
    }

    return UACPI_ITERATION_DECISION_CONTINUE;
}

static acpi_runtime_state_t fail_initialization(uacpi_status error)
{
    runtime.status = acpi_host_status(error);
    runtime.firmware_error = error;
    error_logger("ACPI: runtime unavailable at stage=%u: %s (%u)\n", (unsigned) runtime.stage, uacpi_status_to_string(error), (unsigned) error);

    // uACPI may already have reset itself after a fatal initialization error.
    // Quiesce any remaining host work before discarding interpreter state.
    uacpi_kernel_wait_for_work_completion();
    uacpi_state_reset();
    runtime.device_count = 0;
    uacpi_status tables = uacpi_setup_early_table_access(uacpi_early_table_buffer, 4096);

    if (tables != UACPI_STATUS_OK)
    {
        error_logger("ACPI: restoring early tables failed: %s\n", uacpi_status_to_string(tables));
    }

    return runtime;
}

void acpi_runtime_host_failed(void)
{
    runtime.stage = ACPI_STAGE_HOST;
    fail_initialization(UACPI_STATUS_OUT_OF_MEMORY);
}

acpi_runtime_state_t platform_acpi_initialize(void)
{
    if (attempted)
    {
        return runtime;
    }

    if (arch_cpu_id() != platform.bsp_id)
    {
        acpi_runtime_state_t unavailable = runtime;
        unavailable.status = ACPI_UNSAFE_CONTEXT;
        return unavailable;
    }

    attempted = true;
    runtime.stage = ACPI_STAGE_HOST;

    acpi_status_t host_status = acpi_host_initialize();

    if (host_status != ACPI_SUCCESS)
    {
        runtime.status = host_status;
        return runtime;
    }

    // SCI services the firmware global lock even before ordinary scheduling.
    arch_enable_interrupts();
    operation_lock = uacpi_kernel_create_mutex();

    if (operation_lock == NULL)
    {
        runtime.status = ACPI_NO_MEMORY;
        return runtime;
    }

    runtime.stage = ACPI_STAGE_SUBSYSTEM;
    uacpi_status status = uacpi_initialize(0);

    if (status != UACPI_STATUS_OK)
    {
        return fail_initialization(status);
    }

    runtime.stage = ACPI_STAGE_NAMESPACE;
    status = uacpi_namespace_load();

    if (status == UACPI_STATUS_OK)
    {
        status = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
    }

    if (status == UACPI_STATUS_OK)
    {
        status = uacpi_install_notify_handler(uacpi_namespace_root(), notify_device, NULL);
    }

    if (status == UACPI_STATUS_OK)
    {
        status = uacpi_namespace_initialize();
    }

    if (status != UACPI_STATUS_OK)
    {
        return fail_initialization(status);
    }

    runtime.stage = ACPI_STAGE_DEVICES;

    if (ARX_ACPI_TEST_FAIL_STAGE == ACPI_STAGE_DEVICES)
    {
        return fail_initialization(UACPI_STATUS_INTERNAL_ERROR);
    }

    status = uacpi_namespace_for_each_child(uacpi_namespace_root(), discover_device, NULL, UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, NULL);

    if (status != UACPI_STATUS_OK)
    {
        return fail_initialization(status);
    }

    runtime.stage = ACPI_STAGE_EVENTS;
    struct acpi_fadt* fadt = NULL;
    status = uacpi_table_fadt(&fadt);

    if (status == UACPI_STATUS_OK && (fadt->flags & ACPI_HW_REDUCED_ACPI) == 0)
    {
        if ((fadt->flags & ACPI_PWR_BUTTON) == 0)
        {
            status = uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, fixed_button, (void*) (uintptr_t) ACPI_EVENT_POWER_BUTTON);
        }

        if (status == UACPI_STATUS_OK && (fadt->flags & ACPI_SLP_BUTTON) == 0)
        {
            status = uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_SLEEP_BUTTON, fixed_button, (void*) (uintptr_t) ACPI_EVENT_SLEEP_BUTTON);
        }

        if (status == UACPI_STATUS_OK)
        {
            status = uacpi_finalize_gpe_initialization();
        }
    }

    if (status == UACPI_STATUS_OK)
    {
        status = uacpi_kernel_wait_for_work_completion();
    }

    if (status != UACPI_STATUS_OK)
    {
        return fail_initialization(status);
    }

    runtime.stage = ACPI_STAGE_READY;
    runtime.status = runtime.incomplete ? ACPI_INCOMPLETE : ACPI_SUCCESS;
    kprintf("ACPI: full runtime initialized devices=%llu incomplete=%u\n", (unsigned long long) runtime.device_count, (unsigned) runtime.incomplete);
    return runtime;
}

acpi_runtime_state_t platform_acpi_state(void)
{
    return runtime;
}

const acpi_device_t* platform_acpi_device(size_t index)
{
    if (runtime.stage != ACPI_STAGE_READY || index >= runtime.device_count)
    {
        return NULL;
    }

    return &devices[index];
}

static acpi_status_t begin_operation(void)
{
    if (runtime.stage != ACPI_STAGE_READY)
    {
        return ACPI_NOT_READY;
    }

    if (!acpi_host_can_call())
    {
        return ACPI_UNSAFE_CONTEXT;
    }

    return acpi_host_status(uacpi_kernel_acquire_mutex(operation_lock, 0xffff));
}

acpi_status_t platform_acpi_refresh(acpi_device_id_t device, uint32_t* flags, uint32_t* firmware_error)
{
    if (flags == NULL || firmware_error == NULL || device_node(device) == NULL)
    {
        return ACPI_INVALID_ARGUMENT;
    }

    acpi_status_t result = begin_operation();

    if (result != ACPI_SUCCESS)
    {
        return result;
    }

    uacpi_status status = uacpi_eval_sta(device_node(device), flags);
    *firmware_error = status;
    uacpi_kernel_release_mutex(operation_lock);
    return acpi_host_status(status);
}

static acpi_status_t query_resources(uacpi_namespace_node* node, bool possible, acpi_resources_t* resources)
{
    uacpi_object* object = NULL;
    uacpi_status status = uacpi_eval_simple_buffer(node, possible ? "_PRS" : "_CRS", &object);

    if (status != UACPI_STATUS_OK)
    {
        resources->firmware_error = status;
        return acpi_host_status(status);
    }

    uacpi_data_view buffer;
    status = uacpi_object_get_buffer(object, &buffer);
    acpi_status_t result = acpi_host_status(status);

    if (status == UACPI_STATUS_OK)
    {
        result = platform_acpi_decode_resources(buffer.const_bytes, buffer.length, resources);

        if (result == ACPI_FIRMWARE_ERROR)
        {
            resources->firmware_error = UACPI_STATUS_AML_INVALID_RESOURCE;
        }
    }

    uacpi_object_unref(object);
    return result;
}

acpi_status_t platform_acpi_resources(acpi_device_id_t device, bool possible, acpi_resources_t* resources)
{
    if (resources == NULL || device_node(device) == NULL || resources->entries != NULL || resources->template_bytes != NULL)
    {
        return ACPI_INVALID_ARGUMENT;
    }

    acpi_status_t result = begin_operation();

    if (result != ACPI_SUCCESS)
    {
        return result;
    }

    result = query_resources(device_node(device), possible, resources);
    uacpi_kernel_release_mutex(operation_lock);
    return result;
}

acpi_status_t platform_acpi_next_event(acpi_event_t* event, uint64_t* lost, uint64_t deadline)
{
    if (event == NULL || lost == NULL)
    {
        return ACPI_INVALID_ARGUMENT;
    }

    if (runtime.stage != ACPI_STAGE_READY)
    {
        return ACPI_NOT_READY;
    }

    for (;;)
    {
        uint64_t sequence = __atomic_load_n(&event_sequence, __ATOMIC_ACQUIRE);
        uint64_t state = arch_save_and_disable_interrupts();
        spinlock_acquire(&event_lock);
        *lost = event_lost;
        bool found = event_count != 0;

        if (found)
        {
            *event = events[event_head];
            event_head = (event_head + 1) % ACPI_EVENT_CAPACITY;
            event_count--;
        }

        spinlock_release(&event_lock);
        arch_restore_interrupts(state);

        if (found)
        {
            classify_notification(event);
            return ACPI_SUCCESS;
        }

        uint64_t now = uacpi_kernel_get_nanoseconds_since_boot();

        if (deadline <= now)
        {
            return ACPI_TIMEOUT;
        }

        if (!acpi_host_wait(&event_sequence, sequence, deadline))
        {
            return ACPI_UNSAFE_CONTEXT;
        }
    }
}

static acpi_status_t link_route(uacpi_namespace_node* link, uint32_t index, acpi_pci_route_t* route)
{
    memset(route, 0, sizeof(*route));
    route->link_device = node_identity(link);
    route->resource_index = index;

    uacpi_resources* resources = NULL;
    uacpi_status status = uacpi_get_current_resources(link, &resources);

    if (status != UACPI_STATUS_OK)
    {
        return acpi_host_status(status);
    }

    uacpi_resource* resource = resources->entries;

    for (uint32_t i = 0; i < index && resource->type != UACPI_RESOURCE_TYPE_END_TAG; i++)
    {
        resource = UACPI_NEXT_RESOURCE(resource);
    }

    acpi_status_t result = route->link_device != 0 ? ACPI_NEEDS_CONFIGURATION : ACPI_INCOMPLETE;
    uint32_t flags = 0;
    status = uacpi_eval_sta(link, &flags);

    if (status != UACPI_STATUS_OK)
    {
        uacpi_free_resources(resources);
        return acpi_host_status(status);
    }

    if ((flags & 2) != 0 && resource->type == UACPI_RESOURCE_TYPE_IRQ && resource->irq.num_irqs == 1 && resource->irq.irqs[0] != 0)
    {
        route->gsi = resource->irq.irqs[0];
        route->level_triggered = resource->irq.triggering == UACPI_TRIGGERING_LEVEL;
        route->active_low = resource->irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
        route->shared = resource->irq.sharing == UACPI_SHARED;
        result = ACPI_SUCCESS;
    }
    else if ((flags & 2) != 0 && resource->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ && resource->extended_irq.num_irqs == 1 && resource->extended_irq.irqs[0] != 0)
    {
        route->gsi = resource->extended_irq.irqs[0];
        route->level_triggered = resource->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
        route->active_low = resource->extended_irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
        route->shared = resource->extended_irq.sharing == UACPI_SHARED;
        result = ACPI_SUCCESS;
    }

    uacpi_free_resources(resources);
    return result;
}

static uacpi_namespace_node* pci_child(uacpi_namespace_node* parent, uint8_t device, uint8_t function)
{
    if (parent == NULL)
    {
        return NULL;
    }

    for (size_t i = 0; i < runtime.device_count; i++)
    {
        acpi_device_t* candidate = &devices[i];

        if (uacpi_namespace_node_parent(nodes[i]) == parent && candidate->has_address && (candidate->address >> 16) == device &&
            ((candidate->address & 0xffff) == function || (candidate->address & 0xffff) == 0xffff))
        {
            return nodes[i];
        }
    }

    return NULL;
}

static acpi_status_t route_from_root(size_t root, uint8_t bus, uint8_t device, uint8_t function, uint8_t pin, acpi_pci_route_t* route)
{
    uacpi_namespace_node* scopes[ACPI_PCI_DEPTH];
    uint8_t path_devices[ACPI_PCI_DEPTH];
    uint8_t path_functions[ACPI_PCI_DEPTH];
    size_t depth = 0;
    uint8_t current_bus = devices[root].pci_bus;
    uint16_t segment = devices[root].pci_segment;
    scopes[0] = nodes[root];

    while (current_bus != bus)
    {
        bool found = false;

        if (depth + 1 == ACPI_PCI_DEPTH)
        {
            return ACPI_INCOMPLETE;
        }

        for (uint8_t slot = 0; slot < 32 && !found; slot++)
        {
            for (uint8_t fn = 0; fn < 8; fn++)
            {
                uint32_t class_revision = 0;
                uint32_t buses = 0;

                if (!arch_acpi_pci_access(segment, current_bus, slot, fn, 8, 4, false, &class_revision) || (class_revision >> 16) != 0x0604 ||
                    !arch_acpi_pci_access(segment, current_bus, slot, fn, 0x18, 4, false, &buses))
                {
                    continue;
                }

                uint8_t secondary = buses >> 8;
                uint8_t subordinate = buses >> 16;

                if (secondary == 0 || secondary <= current_bus || secondary > bus || subordinate < bus)
                {
                    continue;
                }

                path_devices[depth] = slot;
                path_functions[depth] = fn;
                scopes[depth + 1] = pci_child(scopes[depth], slot, fn);
                depth++;
                current_bus = secondary;
                found = true;
                break;
            }
        }

        if (!found)
        {
            return ACPI_NOT_FOUND;
        }
    }

    path_devices[depth] = device;
    path_functions[depth] = function;

    for (;;)
    {
        uacpi_pci_routing_table* table = NULL;
        uacpi_status status = scopes[depth] != NULL ? uacpi_get_pci_routing_table(scopes[depth], &table) : UACPI_STATUS_NOT_FOUND;

        if (status == UACPI_STATUS_OK)
        {
            for (size_t i = 0; i < table->num_entries; i++)
            {
                uacpi_pci_routing_table_entry* entry = &table->entries[i];

                if ((entry->address >> 16) != path_devices[depth] || entry->pin != pin ||
                    ((entry->address & 0xffff) != 0xffff && (entry->address & 0xffff) != path_functions[depth]))
                {
                    continue;
                }

                acpi_status_t result = ACPI_SUCCESS;

                if (entry->source != NULL)
                {
                    result = link_route(entry->source, entry->index, route);
                }
                else
                {
                    *route = (acpi_pci_route_t) { entry->index, true, true, true };
                }

                uacpi_free_pci_routing_table(table);
                return result;
            }

            uacpi_free_pci_routing_table(table);
        }
        else if (status != UACPI_STATUS_NOT_FOUND)
        {
            return acpi_host_status(status);
        }

        if (depth == 0)
        {
            return ACPI_NOT_FOUND;
        }

        pin = (pin + path_devices[depth]) % 4;
        depth--;
    }
}

acpi_status_t platform_acpi_route_pci(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint8_t pin, acpi_pci_route_t* route)
{
    if (route == NULL || device >= 32 || function >= 8 || pin >= 4)
    {
        return ACPI_INVALID_ARGUMENT;
    }

    acpi_status_t result = begin_operation();

    if (result != ACPI_SUCCESS)
    {
        return result;
    }

    result = ACPI_NOT_FOUND;

    for (size_t i = 0; i < runtime.device_count; i++)
    {
        if (!devices[i].pci_root || devices[i].pci_segment != segment)
        {
            continue;
        }

        result = route_from_root(i, bus, device, function, pin, route);

        if (result != ACPI_NOT_FOUND)
        {
            break;
        }
    }

    uacpi_kernel_release_mutex(operation_lock);
    return result;
}

acpi_status_t platform_acpi_reset(void)
{
    acpi_status_t result = begin_operation();

    if (result != ACPI_SUCCESS)
    {
        return result;
    }

    result = acpi_host_status(uacpi_reboot());
    uacpi_kernel_release_mutex(operation_lock);
    return result;
}

acpi_status_t platform_acpi_configure_link(acpi_device_id_t identity, uint32_t resource_index, uint32_t gsi)
{
    uacpi_namespace_node* node = device_node(identity);

    if (node == NULL || gsi == 0)
    {
        return ACPI_INVALID_ARGUMENT;
    }

    acpi_status_t result = begin_operation();

    if (result != ACPI_SUCCESS)
    {
        return result;
    }

    const char* link_ids[] = { "PNP0C0F", NULL };
    acpi_resources_t possible = {0};
    acpi_resources_t current = {0};
    const acpi_resource_t* chosen = NULL;

    if (!uacpi_device_matches_pnp_id(node, link_ids))
    {
        result = ACPI_INVALID_ARGUMENT;
        goto finish;
    }

    result = query_resources(node, true, &possible);

    if (result != ACPI_SUCCESS)
    {
        goto finish;
    }

    result = query_resources(node, false, &current);

    if (result != ACPI_SUCCESS)
    {
        goto finish;
    }

    bool alternatives = false;
    size_t current_irq_count = 0;
    size_t alternative_irq_count = 0;

    for (size_t i = 0; i < current.count; i++)
    {
        if (current.entries[i].kind == ARX_ACPI_RESOURCE_IRQ)
        {
            current_irq_count++;
        }
    }

    for (size_t i = 0; i < possible.count; i++)
    {
        if (possible.entries[i].kind == ARX_ACPI_RESOURCE_DEPENDENT_START)
        {
            alternatives = true;
            break;
        }
    }

    if (alternatives && current_irq_count != 1)
    {
        result = ACPI_UNSUPPORTED;
        goto finish;
    }

    // A link's alternatives may choose an IRQ, but never silently allocate
    // unrelated I/O/memory/DMA resources on behalf of an interrupt allocator.
    for (size_t i = 0; i < possible.count; i++)
    {
        const acpi_resource_t* resource = &possible.entries[i];

        if (resource->kind == ARX_ACPI_RESOURCE_DEPENDENT_START)
        {
            alternative_irq_count = 0;
        }

        if (alternatives && resource->kind == ARX_ACPI_RESOURCE_IRQ)
        {
            alternative_irq_count++;

            if (alternative_irq_count > 1)
            {
                result = ACPI_UNSUPPORTED;
                goto finish;
            }
        }

        if (resource->kind != ARX_ACPI_RESOURCE_IRQ && resource->kind != ARX_ACPI_RESOURCE_DEPENDENT_START &&
            resource->kind != ARX_ACPI_RESOURCE_DEPENDENT_END && resource->kind != ARX_ACPI_RESOURCE_END)
        {
            result = ACPI_UNSUPPORTED;
            goto finish;
        }

        for (size_t irq = 0; irq < resource->interrupt_count; irq++)
        {
            if (resource->interrupts[irq] == gsi && (alternatives || i == resource_index))
            {
                chosen = resource;
            }
        }
    }

    if (chosen == NULL)
    {
        result = ACPI_INVALID_ARGUMENT;
        goto finish;
    }

    if (resource_index >= current.count || current.entries[resource_index].kind != ARX_ACPI_RESOURCE_IRQ)
    {
        result = ACPI_INVALID_ARGUMENT;
        goto finish;
    }

    acpi_resource_t* descriptor = &current.entries[resource_index];
    uint8_t* bytes = current.template_bytes + descriptor->offset;
    uint8_t tag = bytes[0];

    if ((tag >> 3) == 4 && (tag & 0x80) == 0 && gsi < 16)
    {
        uint16_t mask = 1u << gsi;
        bytes[1] = mask;
        bytes[2] = mask >> 8;

        if ((tag & 7) == 3)
        {
            bytes[3] = (bytes[3] & ~0x19u) | (!chosen->level_triggered ? 1 : 0) |
                       (chosen->active_low ? 8 : 0) | (chosen->shared ? 16 : 0);
        }
        else if (chosen->level_triggered || chosen->active_low || chosen->shared)
        {
            result = ACPI_UNSUPPORTED;
            goto finish;
        }
    }
    else if (tag == 0x89 && bytes[4] == 1)
    {
        bytes[3] = (bytes[3] & ~0x0eu) | (!chosen->level_triggered ? 2 : 0) |
                   (chosen->active_low ? 4 : 0) | (chosen->shared ? 8 : 0);

        for (size_t i = 0; i < 4; i++)
        {
            bytes[5 + i] = gsi >> (i * 8);
        }
    }
    else
    {
        result = ACPI_UNSUPPORTED;
        goto finish;
    }

    // A zero EndTag checksum means no checksum; changing IRQs invalidates any
    // prior checksum. Preserve descriptor count, ordering and byte lengths.
    const acpi_resource_t* end = &current.entries[current.count - 1];
    current.template_bytes[end->offset + 1] = 0;
    uacpi_data_view view = { .bytes = current.template_bytes, .length = current.template_size };
    uacpi_object* buffer = uacpi_object_create_buffer(view);

    if (buffer == NULL)
    {
        result = ACPI_NO_MEMORY;
        goto finish;
    }

    uacpi_object_array arguments = { .objects = &buffer, .count = 1 };
    uacpi_status status = uacpi_execute(node, "_SRS", &arguments);
    uacpi_object_unref(buffer);
    result = acpi_host_status(status);

    if (result == ACPI_SUCCESS)
    {
        acpi_pci_route_t actual;
        result = link_route(node, resource_index, &actual);

        if (result == ACPI_SUCCESS && (actual.gsi != gsi || actual.level_triggered != chosen->level_triggered ||
            actual.active_low != chosen->active_low || actual.shared != chosen->shared))
        {
            result = ACPI_FIRMWARE_ERROR;
        }
    }

finish:
    platform_acpi_free_resources(&possible);
    platform_acpi_free_resources(&current);
    uacpi_kernel_release_mutex(operation_lock);
    return result;
}

acpi_status_t platform_acpi_power_off(void)
{
    acpi_status_t result = begin_operation();

    if (result != ACPI_SUCCESS)
    {
        return result;
    }

    uacpi_status status = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);

    if (status == UACPI_STATUS_OK)
    {
        uint64_t state = arch_save_and_disable_interrupts();
        status = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
        arch_restore_interrupts(state);
    }

    uacpi_kernel_release_mutex(operation_lock);
    return acpi_host_status(status);
}
