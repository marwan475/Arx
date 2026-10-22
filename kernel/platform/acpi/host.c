#include <acpi/host.h>
#include <arch/arch.h>
#include <klib/klib.h>
#include <layers/DispatcherBridge.h>
#include <memory/vmm.h>
#include <platform.h>

#define ACPI_WORK_CAPACITY 256
#define ACPI_HOST_WORKERS 4
#define ACPI_IRQ_CAPACITY 16

typedef struct acpi_host_sync
{
    uint32_t count;
    uint64_t sequence;
} acpi_host_sync_t;

typedef struct acpi_host_work
{
    uacpi_work_handler handler;
    void* context;
} acpi_host_work_t;

typedef struct acpi_host_irq
{
    uacpi_interrupt_handler handler;
    void* context;
    uint32_t irq;
    uint32_t vector;
    uint32_t active;
} acpi_host_irq_t;

typedef struct acpi_boot_context
{
    struct arch_task_context context;
    void* stack;
} acpi_boot_context_t;

static acpi_boot_context_t boot_contexts[ACPI_HOST_WORKERS + 1];
static unsigned boot_current;
static bool host_initialized;
static bool scheduled;
static bool workers_created;
static uint32_t interrupt_depth[BOOT_SMP_MAX_CPUS];
static acpi_host_work_t work_queue[ACPI_WORK_CAPACITY];
static size_t work_head;
static size_t work_count;
static size_t work_active;
static uacpi_thread_id executing_threads[ACPI_HOST_WORKERS];
static uint64_t work_sequence;
static spinlock_t work_lock;
static acpi_host_irq_t interrupts[ACPI_IRQ_CAPACITY];
static spinlock_t interrupt_lock;

static uint64_t host_now(void)
{
    uint64_t nanoseconds = 0;
    arch_time_read_monotonic(&nanoseconds);
    return nanoseconds;
}

static uint64_t host_deadline(uint64_t milliseconds)
{
    uint64_t now = host_now();

    if (milliseconds > (UINT64_MAX - now) / 1000000)
    {
        return UINT64_MAX;
    }

    return now + milliseconds * 1000000;
}

void platform_acpi_interrupt_enter(void)
{
    __atomic_add_fetch(&interrupt_depth[arch_cpu_id()], 1, __ATOMIC_RELAXED);
}

void platform_acpi_interrupt_leave(void)
{
    __atomic_sub_fetch(&interrupt_depth[arch_cpu_id()], 1, __ATOMIC_RELAXED);
}

bool acpi_host_can_call(void)
{
    if (!host_initialized || !arch_interrupts_enabled() || __atomic_load_n(&interrupt_depth[arch_cpu_id()], __ATOMIC_RELAXED) != 0 || platform.cpus[arch_cpu_id()].preemption_depth != 0)
    {
        return false;
    }

    if (__atomic_load_n(&scheduled, __ATOMIC_ACQUIRE))
    {
        return dispatcher_acpi_can_wait();
    }

    return arch_cpu_id() == platform.bsp_id;
}

void acpi_host_yield(void)
{
    if (__atomic_load_n(&scheduled, __ATOMIC_ACQUIRE))
    {
        uint64_t sequence = __atomic_load_n(&work_sequence, __ATOMIC_ACQUIRE);
        dispatcher_acpi_wait(&work_sequence, sequence, host_deadline(1));
        return;
    }

    unsigned previous = boot_current;
    unsigned next = (previous + 1) % (ACPI_HOST_WORKERS + 1);
    boot_current = next;
    arch_bootstrap_switch_context(&boot_contexts[previous].context, &boot_contexts[next].context);
    boot_current = previous;
}

void acpi_host_wake(uint64_t* sequence)
{
    __atomic_add_fetch(sequence, 1, __ATOMIC_RELEASE);

    if (__atomic_load_n(&scheduled, __ATOMIC_ACQUIRE) && arch_interrupts_enabled() &&
        platform.cpus[arch_cpu_id()].preemption_depth == 0 && __atomic_load_n(&interrupt_depth[arch_cpu_id()], __ATOMIC_RELAXED) == 0)
    {
        dispatcher_acpi_wake(sequence);
    }

    // The scheduler heartbeat observes changed wait sequences on every CPU.
    // IRQ/spinlock callers publish readiness without entering scheduler locks
    // that may be held by the interrupted task.
}

bool acpi_host_wait(const uint64_t* sequence, uint64_t expected, uint64_t deadline)
{
    if (!acpi_host_can_call())
    {
        return false;
    }

    if (__atomic_load_n(&scheduled, __ATOMIC_ACQUIRE))
    {
        return dispatcher_acpi_wait(sequence, expected, deadline);
    }

    while (__atomic_load_n(sequence, __ATOMIC_ACQUIRE) == expected && host_now() < deadline)
    {
        acpi_host_yield();
        arch_pause();
    }

    return true;
}

static bool host_take_work(acpi_host_work_t* work)
{
    uint64_t state = arch_save_and_disable_interrupts();
    spinlock_acquire(&work_lock);
    bool found = work_count != 0;

    if (found)
    {
        *work = work_queue[work_head];
        work_head = (work_head + 1) % ACPI_WORK_CAPACITY;
        work_count--;
        work_active++;
    }

    spinlock_release(&work_lock);
    arch_restore_interrupts(state);
    return found;
}

static void host_worker(void* argument)
{
    bool normal_task = argument != NULL;
    unsigned worker_index = normal_task ? (unsigned) (uintptr_t) argument - 1 : boot_current - 1;

    if (normal_task)
    {
        __atomic_store_n(&scheduled, true, __ATOMIC_RELEASE);
    }

    for (;;)
    {
        uint64_t sequence = __atomic_load_n(&work_sequence, __ATOMIC_ACQUIRE);
        acpi_host_work_t work;

        if (!host_take_work(&work))
        {
            if (normal_task)
            {
                acpi_host_wait(&work_sequence, sequence, UINT64_MAX);
            }
            else
            {
                acpi_host_yield();
            }

            continue;
        }

        __atomic_store_n(&executing_threads[worker_index], uacpi_kernel_get_thread_id(), __ATOMIC_RELEASE);
        work.handler(work.context);
        __atomic_store_n(&executing_threads[worker_index], NULL, __ATOMIC_RELEASE);

        uint64_t state = arch_save_and_disable_interrupts();
        spinlock_acquire(&work_lock);
        work_active--;
        spinlock_release(&work_lock);
        arch_restore_interrupts(state);
        acpi_host_wake(&work_sequence);
    }
}

acpi_status_t acpi_host_initialize(void)
{
    uint64_t now;

    if (host_initialized)
    {
        return ACPI_SUCCESS;
    }

    if (!arch_task_context_supported() || !arch_time_read_monotonic(&now))
    {
        return ACPI_UNSUPPORTED;
    }

    for (unsigned i = 1; i <= ACPI_HOST_WORKERS; i++)
    {
        boot_contexts[i].stack = vmalloc(CPU_KERNEL_STACK_SIZE + PAGE_SIZE);

        if (boot_contexts[i].stack == NULL)
        {
            for (unsigned allocated = 1; allocated < i; allocated++)
            {
                vfree(boot_contexts[allocated].stack);
                boot_contexts[allocated].stack = NULL;
            }

            return ACPI_NO_MEMORY;
        }

        vmm_unmap_range((virt_addr_t) boot_contexts[i].stack, PAGE_SIZE, vmm_kernel_space());
        arch_init_context(&boot_contexts[i].context, (uint8_t*) boot_contexts[i].stack + PAGE_SIZE + CPU_KERNEL_STACK_SIZE, host_worker, NULL);
    }

    host_initialized = true;
    return ACPI_SUCCESS;
}

bool platform_acpi_start_workers(void)
{
    if (!host_initialized)
    {
        return false;
    }

    if (workers_created)
    {
        return true;
    }

    // Drain bootstrap callbacks before abandoning their execution stacks.
    if (uacpi_kernel_wait_for_work_completion() != UACPI_STATUS_OK)
    {
        acpi_runtime_host_failed();
        return false;
    }

    for (unsigned i = 0; i < ACPI_HOST_WORKERS; i++)
    {
        if (!dispatcher_acpi_create_worker(host_worker, (void*) (uintptr_t) (i + 1)))
        {
            acpi_runtime_host_failed();
            return false;
        }
    }

    workers_created = true;
    // Commit the execution-mode handoff before ActivateScheduling can dispatch
    // any normal caller, even if it runs before the first firmware worker.
    __atomic_store_n(&scheduled, true, __ATOMIC_RELEASE);

    for (unsigned i = 1; i <= ACPI_HOST_WORKERS; i++)
    {
        vfree(boot_contexts[i].stack);
        boot_contexts[i].stack = NULL;
    }

    return true;
}

void* uacpi_kernel_alloc(uacpi_size size)
{
    if (size > SIZE_MAX - 16)
    {
        return NULL;
    }

    bool large = size + 16 > HEAP_MAX_OBJECT_SIZE;
    uint64_t* allocation = large ? vmalloc(size + 16) : kmalloc(size + 16);

    if (allocation == NULL)
    {
        return NULL;
    }

    allocation[0] = large;
    allocation[1] = size;
    return allocation + 2;
}

void uacpi_kernel_free(void* memory)
{
    if (memory == NULL)
    {
        return;
    }

    uint64_t* allocation = (uint64_t*) memory - 2;

    if (allocation[0])
    {
        vfree(allocation);
    }
    else
    {
        kfree(allocation);
    }
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    return host_now();
}

void uacpi_kernel_stall(uacpi_u8 microseconds)
{
    uint64_t end = host_now() + (uint64_t) microseconds * 1000;

    while (host_now() < end)
    {
        arch_pause();
    }
}

void uacpi_kernel_sleep(uacpi_u64 milliseconds)
{
    uint64_t deadline = host_deadline(milliseconds);
    uint64_t sequence = 0;
    acpi_host_wait(&sequence, 0, deadline);
}

uacpi_handle uacpi_kernel_create_mutex(void)
{
    return kzalloc(sizeof(acpi_host_sync_t));
}

void uacpi_kernel_free_mutex(uacpi_handle mutex)
{
    kfree(mutex);
}

uacpi_handle uacpi_kernel_create_event(void)
{
    return kzalloc(sizeof(acpi_host_sync_t));
}

void uacpi_kernel_free_event(uacpi_handle event)
{
    kfree(event);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    if (!__atomic_load_n(&scheduled, __ATOMIC_ACQUIRE))
    {
        return (void*) (uintptr_t) (boot_current + 1);
    }

    return (void*) (uintptr_t) (dispatcher_acpi_thread_id() + ACPI_HOST_WORKERS + 2);
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
    return arch_save_and_disable_interrupts();
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state)
{
    arch_restore_interrupts(state);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout)
{
    acpi_host_sync_t* mutex = handle;
    uint64_t deadline = timeout == 0xffff ? UINT64_MAX : host_deadline(timeout);

    for (;;)
    {
        uint64_t sequence = __atomic_load_n(&mutex->sequence, __ATOMIC_ACQUIRE);
        uint32_t expected = 0;

        if (__atomic_compare_exchange_n(&mutex->count, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        {
            return UACPI_STATUS_OK;
        }

        if (timeout == 0 || host_now() >= deadline)
        {
            return UACPI_STATUS_TIMEOUT;
        }

        if (!acpi_host_wait(&mutex->sequence, sequence, deadline))
        {
            return UACPI_STATUS_DENIED;
        }
    }
}

void uacpi_kernel_release_mutex(uacpi_handle handle)
{
    acpi_host_sync_t* mutex = handle;
    __atomic_store_n(&mutex->count, 0, __ATOMIC_RELEASE);
    acpi_host_wake(&mutex->sequence);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout)
{
    acpi_host_sync_t* event = handle;
    uint64_t deadline = timeout == 0xffff ? UINT64_MAX : host_deadline(timeout);

    for (;;)
    {
        uint64_t sequence = __atomic_load_n(&event->sequence, __ATOMIC_ACQUIRE);
        uint32_t count = __atomic_load_n(&event->count, __ATOMIC_ACQUIRE);

        if (count != 0 && __atomic_compare_exchange_n(&event->count, &count, count - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        {
            return UACPI_TRUE;
        }

        if (timeout == 0 || host_now() >= deadline || !acpi_host_wait(&event->sequence, sequence, deadline))
        {
            return UACPI_FALSE;
        }
    }
}

void uacpi_kernel_signal_event(uacpi_handle handle)
{
    acpi_host_sync_t* event = handle;
    uint32_t count = __atomic_load_n(&event->count, __ATOMIC_RELAXED);

    while (count != UINT32_MAX && !__atomic_compare_exchange_n(&event->count, &count, count + 1, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
    {
        arch_pause();
    }

    acpi_host_wake(&event->sequence);
}

void uacpi_kernel_reset_event(uacpi_handle handle)
{
    acpi_host_sync_t* event = handle;
    __atomic_store_n(&event->count, 0, __ATOMIC_RELEASE);
}

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    return kzalloc(sizeof(spinlock_t));
}

void uacpi_kernel_free_spinlock(uacpi_handle lock)
{
    kfree(lock);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle lock)
{
    uint64_t state = arch_save_and_disable_interrupts();
    spinlock_acquire(lock);
    return state;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle lock, uacpi_cpu_flags state)
{
    spinlock_release(lock);
    arch_restore_interrupts(state);
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler, uacpi_handle context)
{
    if (handler == NULL || (type != UACPI_WORK_GPE_EXECUTION && type != UACPI_WORK_NOTIFICATION))
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint64_t state = arch_save_and_disable_interrupts();
    spinlock_acquire(&work_lock);

    if (work_count == ACPI_WORK_CAPACITY)
    {
        spinlock_release(&work_lock);
        arch_restore_interrupts(state);
        return UACPI_STATUS_OUT_OF_MEMORY;
    }

    size_t tail = (work_head + work_count) % ACPI_WORK_CAPACITY;
    work_queue[tail] = (acpi_host_work_t) { handler, context };
    work_count++;
    spinlock_release(&work_lock);
    arch_restore_interrupts(state);
    acpi_host_wake(&work_sequence);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    if (!acpi_host_can_call())
    {
        return UACPI_STATUS_DENIED;
    }

    uacpi_thread_id current = uacpi_kernel_get_thread_id();

    for (size_t i = 0; i < ACPI_HOST_WORKERS; i++)
    {
        if (__atomic_load_n(&executing_threads[i], __ATOMIC_ACQUIRE) == current)
        {
            // Waiting for this callback itself could never complete.
            return UACPI_STATUS_DENIED;
        }
    }

    for (size_t i = 0; i < ACPI_IRQ_CAPACITY; i++)
    {
        while (__atomic_load_n(&interrupts[i].active, __ATOMIC_ACQUIRE) != 0)
        {
            acpi_host_yield();
        }
    }

    for (;;)
    {
        uint64_t sequence = __atomic_load_n(&work_sequence, __ATOMIC_ACQUIRE);
        uint64_t state = arch_save_and_disable_interrupts();
        spinlock_acquire(&work_lock);
        bool done = work_count == 0 && work_active == 0;
        spinlock_release(&work_lock);
        arch_restore_interrupts(state);

        if (done)
        {
            return UACPI_STATUS_OK;
        }

        if (!acpi_host_wait(&work_sequence, sequence, UINT64_MAX))
        {
            return UACPI_STATUS_DENIED;
        }
    }
}

bool acpi_host_dispatch_interrupt(uint32_t vector)
{
    for (size_t i = 0; i < ACPI_IRQ_CAPACITY; i++)
    {
        acpi_host_irq_t* irq = &interrupts[i];
        uacpi_interrupt_handler handler = __atomic_load_n(&irq->handler, __ATOMIC_ACQUIRE);

        if (handler != NULL && irq->vector == vector)
        {
            __atomic_add_fetch(&irq->active, 1, __ATOMIC_ACQUIRE);
            platform_acpi_interrupt_enter();
            handler(irq->context);
            platform_acpi_interrupt_leave();
            __atomic_sub_fetch(&irq->active, 1, __ATOMIC_RELEASE);
            return true;
        }
    }

    return false;
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 number, uacpi_interrupt_handler handler, uacpi_handle context, uacpi_handle* output)
{
    if (handler == NULL || output == NULL || arch_cpu_id() != platform.bsp_id)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint64_t state = arch_save_and_disable_interrupts();
    spinlock_acquire(&interrupt_lock);
    acpi_host_irq_t* slot = NULL;

    for (size_t i = 0; i < ACPI_IRQ_CAPACITY; i++)
    {
        if (interrupts[i].handler != NULL && interrupts[i].irq == number)
        {
            spinlock_release(&interrupt_lock);
            arch_restore_interrupts(state);
            return UACPI_STATUS_ALREADY_EXISTS;
        }

        if (interrupts[i].handler == NULL)
        {
            slot = &interrupts[i];
        }
    }

    uint32_t vector = slot != NULL ? arch_acpi_route_interrupt(number) : 0;

    if (vector == 0)
    {
        spinlock_release(&interrupt_lock);
        arch_restore_interrupts(state);
        return UACPI_STATUS_UNIMPLEMENTED;
    }

    slot->irq = number;
    slot->vector = vector;
    slot->context = context;
    __atomic_store_n(&slot->handler, handler, __ATOMIC_RELEASE);
    *output = slot;
    arch_acpi_mask_interrupt(vector, false);
    spinlock_release(&interrupt_lock);
    arch_restore_interrupts(state);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle handle)
{
    acpi_host_irq_t* slot = NULL;

    for (size_t i = 0; i < ACPI_IRQ_CAPACITY; i++)
    {
        if (handle == &interrupts[i])
        {
            slot = &interrupts[i];
        }
    }

    if (slot == NULL || handler == NULL || arch_cpu_id() != platform.bsp_id || !acpi_host_can_call())
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint64_t state = arch_save_and_disable_interrupts();
    spinlock_acquire(&interrupt_lock);

    if (slot->handler != handler)
    {
        spinlock_release(&interrupt_lock);
        arch_restore_interrupts(state);
        return UACPI_STATUS_NOT_FOUND;
    }

    arch_acpi_mask_interrupt(slot->vector, true);
    __atomic_store_n(&slot->handler, NULL, __ATOMIC_RELEASE);
    spinlock_release(&interrupt_lock);
    arch_restore_interrupts(state);

    while (__atomic_load_n(&slot->active, __ATOMIC_ACQUIRE) != 0)
    {
        acpi_host_yield();
    }

    return UACPI_STATUS_OK;
}

typedef struct acpi_io_range
{
    uint16_t base;
    size_t length;
} acpi_io_range_t;

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length, uacpi_handle* output)
{
    if (output == NULL || length == 0 || base > UINT16_MAX || length > 0x10000 - base)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    acpi_io_range_t* range = kmalloc(sizeof(*range));

    if (range == NULL)
    {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }

    range->base = base;
    range->length = length;
    *output = range;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    kfree(handle);
}

static uacpi_status host_io_access(uacpi_handle handle, size_t offset, uint8_t width, bool write, uint32_t* value)
{
    acpi_io_range_t* range = handle;

    if (range == NULL || offset > range->length || width > range->length - offset)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    bool success = write ? arch_acpi_io_write(range->base + offset, width, *value) : arch_acpi_io_read(range->base + offset, width, value);
    return success ? UACPI_STATUS_OK : UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle* output)
{
    if (output == NULL || address.device >= 32 || address.function >= 8)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uacpi_pci_address* copy = kmalloc(sizeof(*copy));

    if (copy == NULL)
    {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }

    *copy = address;
    *output = copy;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle)
{
    kfree(handle);
}

static uacpi_status host_pci_access(uacpi_handle handle, size_t offset, uint8_t width, bool write, uint32_t* value)
{
    uacpi_pci_address* address = handle;

    if (address == NULL || offset > 4096 - width || (offset & (width - 1)) != 0)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    bool success = arch_acpi_pci_access(address->segment, address->bus, address->device, address->function, offset, width, write, value);
    return success ? UACPI_STATUS_OK : UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8* output)
{
    if (output == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint32_t value = 0;
    uacpi_status status = host_io_access(handle, offset, 1, false, &value);

    if (status == UACPI_STATUS_OK)
    {
        *output = value;
    }

    return status;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 input)
{
    uint32_t value = input;
    return host_io_access(handle, offset, 1, true, &value);
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16* output)
{
    if (output == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint32_t value = 0;
    uacpi_status status = host_io_access(handle, offset, 2, false, &value);

    if (status == UACPI_STATUS_OK)
    {
        *output = value;
    }

    return status;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 input)
{
    uint32_t value = input;
    return host_io_access(handle, offset, 2, true, &value);
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32* output)
{
    if (output == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint32_t value = 0;
    uacpi_status status = host_io_access(handle, offset, 4, false, &value);

    if (status == UACPI_STATUS_OK)
    {
        *output = value;
    }

    return status;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 input)
{
    uint32_t value = input;
    return host_io_access(handle, offset, 4, true, &value);
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8* output)
{
    if (output == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint32_t value = 0;
    uacpi_status status = host_pci_access(handle, offset, 1, false, &value);

    if (status == UACPI_STATUS_OK)
    {
        *output = value;
    }

    return status;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 input)
{
    uint32_t value = input;
    return host_pci_access(handle, offset, 1, true, &value);
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16* output)
{
    if (output == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint32_t value = 0;
    uacpi_status status = host_pci_access(handle, offset, 2, false, &value);

    if (status == UACPI_STATUS_OK)
    {
        *output = value;
    }

    return status;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 input)
{
    uint32_t value = input;
    return host_pci_access(handle, offset, 2, true, &value);
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32* output)
{
    if (output == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    uint32_t value = 0;
    uacpi_status status = host_pci_access(handle, offset, 4, false, &value);

    if (status == UACPI_STATUS_OK)
    {
        *output = value;
    }

    return status;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 input)
{
    uint32_t value = input;
    return host_pci_access(handle, offset, 4, true, &value);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* request)
{
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT)
    {
        kprintf("ACPI: firmware breakpoint\n");
        return UACPI_STATUS_OK;
    }

    acpi_event_t event = { .kind = ACPI_EVENT_FIRMWARE_FATAL, .value = request->fatal.code };
    acpi_host_publish_event(&event);
    error_logger("ACPI: firmware Fatal type=%u code=%u argument=%llu\n", request->fatal.type, request->fatal.code, (unsigned long long) request->fatal.arg);
    return UACPI_STATUS_INTERNAL_ERROR;
}

acpi_status_t acpi_host_status(uacpi_status status)
{
    switch (status)
    {
        case UACPI_STATUS_OK:
            return ACPI_SUCCESS;
        case UACPI_STATUS_NOT_FOUND:
            return ACPI_NOT_FOUND;
        case UACPI_STATUS_OUT_OF_MEMORY:
            return ACPI_NO_MEMORY;
        case UACPI_STATUS_INVALID_ARGUMENT:
            return ACPI_INVALID_ARGUMENT;
        case UACPI_STATUS_UNIMPLEMENTED:
        case UACPI_STATUS_COMPILED_OUT:
            return ACPI_UNSUPPORTED;
        case UACPI_STATUS_TIMEOUT:
        case UACPI_STATUS_HARDWARE_TIMEOUT:
            return ACPI_TIMEOUT;
        default:
            return ACPI_FIRMWARE_ERROR;
    }
}
