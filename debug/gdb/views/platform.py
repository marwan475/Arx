"""Stored-RAM platform v1 views. Native contracts live in PlatformStats.h/guide."""
from framework.results import ReadFailure, Result

CPU_SEMANTICS = "arx.platform.cpus"
PCI_SEMANTICS = "arx.platform.pci"
CPU_CAPACITY = 8
PCI_CAPACITY = 256
CPU_STAGES = {0: "not-started", 1: "arch-initializing", 2: "arch-initialized",
              3: "kernel-stack-arrived"}
MAILBOX_TYPES = {0: "NONE", 1: "RESCHEDULE", 2: "INVALIDATE_TLB", 3: "EXCEPTION"}
PCI_OUTCOMES = {0: "not-started", 1: "enumerating", 2: "succeeded",
                3: "no-regions", 4: "failed", 5: "unsupported"}


def root(memory, descriptor, type_name, capacity, budget):
    if int(descriptor["version"]) != 1:
        raise ReadFailure("unsupported platform view version")
    if int(descriptor["rootSize"]) != memory.sizeof(type_name):
        raise ReadFailure("mismatched view root size")
    if int(descriptor["capacity"]) != capacity or int(descriptor["count"]) != 0:
        raise ReadFailure("invalid platform descriptor bounds")
    value = memory.read(int(descriptor["root"]), type_name, budget)
    if int(value["capacity"]) != capacity:
        raise ReadFailure("invalid stored allocation capacity")
    if not int(value["source"]):
        raise ReadFailure("unavailable: null platform source")
    return value


def inventory(memory, source, pointer_field, count_field, capacity, budget):
    count = int(memory.field(source, "platform_t", count_field, budget))
    address = int(memory.field(source, "platform_t", pointer_field, budget))
    if not 0 <= count <= capacity:
        raise ReadFailure("invalid collection length/capacity")
    if count and not address:
        raise ReadFailure("null root with nonzero count")
    return address, count


def row(result, budget, name, text, type_name="stored observation"):
    budget.take_entry()
    result.items.append({"path": name, "type": type_name, "text": text})


def unknown(mapping, number):
    return mapping.get(number, "unknown(%d)" % number)


def cpus(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    stats = root(memory, descriptor, "arx_cpu_debug_stats_t", CPU_CAPACITY, budget)
    source = int(stats["source"])
    result = Result(source=int(descriptor["root"]), type_name="cpu_info_t",
                    count_semantics="CPU slots in configured count; one entry per CPU/topology relation")
    try:
        if int(stats["tableReady"]) != 1:
            raise ReadFailure("unavailable: CPU table not ready")
        address, count = inventory(memory, source, "cpus", "cpu_count", CPU_CAPACITY, budget)
        if count == 0:
            raise ReadFailure("invalid configured CPU count: zero")
        arch = int(memory.field(source, "platform_t", "arch", budget))
        if arch != 0:
            raise ReadFailure("unsupported platform architecture: %d" % arch)
        if "--slot" in selectors and selectors["--slot"] >= count:
            raise ReadFailure("CPU slot outside configured count")
        bsp = int(memory.field(source, "platform_t", "bsp_id", budget))
        arrived = int(memory.field(source, "platform_t", "cpus_initialized", budget))
        handoff = int(memory.field(source, "platform_t", "bsp_kmain_exited", budget))
        row(result, budget, "cpuInventory",
            "root=%#x count=%d capacity=%d bspSlot=%d stackBarrierCount=%d bspKmainExited=%d"
            % (address, count, CPU_CAPACITY, bsp, arrived, handoff))
        if bsp >= count or arrived > count:
            result.fail("invalid BSP slot or stack-barrier count")
        shared = memory.field(source, "platform_t", "arch_info", budget)
        row(result, budget, "interruptTopology",
            "ioapicPresent=%d initialized=%d id=%d gsiBase=%d address=%#x redirections=%d/%d"
            % tuple(int(shared[field]) for field in
                    ("acpi_has_ioapic", "ioapic_initialized", "acpi_ioapic_id",
                     "acpi_ioapic_gsi_base", "acpi_ioapic_base_addr",
                     "ioapic_redir_count", "ioapic_max_redir")))
        relationships = {}
        selected = []
        seen_ids = set()
        for slot in range(count):
            if "--slot" in selectors and selectors["--slot"] != slot:
                continue
            # The physical bound is fixed even when a logical-ID search must
            # examine unselected slots. All reads also consume the byte budget.
            location = address + slot * memory.sizeof("cpu_info_t")
            try:
                cpu = memory.read(location, "cpu_info_t", budget)
            except ReadFailure as error:
                result.fail("slot %d: %s" % (slot, error))
                continue
            logical_id = int(cpu["id"])
            if logical_id in seen_ids:
                result.fail("duplicate logical CPU ID: %d" % logical_id)
            seen_ids.add(logical_id)
            if "--cpu" in selectors and selectors["--cpu"] != logical_id:
                continue
            selected.append(slot)
            ipi = cpu["ipi_request_data"]
            request = int(ipi["type"])
            arch_info = cpu["arch_info"]
            stack_base = int(cpu["kernel_stack_base"])
            stack_size = int(cpu["kernel_stack_size"])
            stack_end = stack_base + stack_size
            if stack_end >= 1 << 64 or (stack_size and not stack_base):
                result.fail("slot %d: invalid kernel-stack bounds" % slot)
            stage = int(stats["stages"][slot])
            text = ("source=%#x slot=%d id=%d hardwareId=%d initialized=%d stage=%s\n"
                    "    preemptionDepth=%d switching=%d reschedulePending=%d syscallActive=%d\n"
                    "    kernelStack=[%#x,%#x) bytes=%d numa=%#x addressSpace=%#x\n"
                    "    lapicPresent=%d lapicAddress=%#x acpiUid=%d lapicId=%d lapicFlags=%#x\n"
                    "    mailbox=%s mailboxLock=%d; task=unavailable (no correlation adapter)"
                    % (location, slot, logical_id, int(cpu["hardware_id"]), int(cpu["initialized"]),
                       unknown(CPU_STAGES, stage), int(cpu["preemption_depth"]),
                       int(cpu["switching_context"]), int(cpu["reschedule_pending"]),
                       int(arch_info["syscall_ctx"]["active"]), stack_base, stack_end, stack_size,
                       int(cpu["numa_node"]), int(cpu["address_space"]),
                       int(arch_info["acpi_has_lapic"]), int(arch_info["acpi_lapic_base_addr"]),
                       int(arch_info["acpi_processor_uid"]), int(arch_info["acpi_lapic_id"]),
                       int(arch_info["acpi_lapic_flags"]), unknown(MAILBOX_TYPES, request),
                       int(cpu["ipi_lock"])))
            if request == 2:
                tlb = ipi["tlb_invalidation"]
                kind = int(tlb["tlb_invalidation_type"])
                start, size = int(tlb["va_start"]), int(tlb["size"])
                text += "\n    TLB request start=%#x size=%d pageFlush=%d kind=%s (no address-space identity)" % (
                    start, size, int(tlb["requires_page_flush"]),
                    unknown({0: "SINGLE_PAGE", 1: "RANGE"}, kind))
                if kind not in (0, 1) or start + size > 1 << 64:
                    result.fail("slot %d: invalid TLB request kind/range" % slot)
            if request not in MAILBOX_TYPES or stage not in CPU_STAGES:
                result.fail("slot %d: unknown mailbox type or CPU stage" % slot)
            row(result, budget, "cpu[%d]" % slot, text)
            for field in ("numa_node", "address_space"):
                pointer = int(cpu[field])
                if pointer:
                    relationships.setdefault((field, pointer), []).append(slot)
        if not selected and selectors and not result.issues:
            result.fail("CPU selector did not match a configured CPU")
        for (kind, pointer), slots in relationships.items():
            row(result, budget, kind + "@%#x" % pointer,
                "cpuSlots=%s; borrowed identity; enrichment unavailable (no correlation adapter)"
                % ",".join(map(str, slots)))
    except ReadFailure as error:
        result.fail(error)
    return result


cpus.selectors = ("--cpu", "--slot")


def pci(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    stats = root(memory, descriptor, "arx_pci_debug_stats_t", PCI_CAPACITY, budget)
    result = Result(source=int(descriptor["root"]), type_name="pci_device_t",
                    count_semantics="captured inventory order; one entry per device; no config reads")
    try:
        outcome = int(stats["outcome"])
        row(result, budget, "inventory", "outcome=%s allocated=%d capacity=%d capacityExhausted=%d"
            % (unknown(PCI_OUTCOMES, outcome), int(stats["allocated"]), PCI_CAPACITY,
               int(stats["capacityExhausted"])))
        if outcome not in (2, 3):
            result.fail("inventory unavailable or partial: " + unknown(PCI_OUTCOMES, outcome))
        if int(stats["capacityExhausted"]):
            result.fail("inventory truncated: allocation capacity exhausted")
        source = int(stats["source"])
        arch = int(memory.field(source, "platform_t", "arch", budget))
        if arch != 0:
            raise ReadFailure("unsupported platform architecture: %d" % arch)
        address, count = inventory(memory, source, "pci_devices", "pci_device_count", PCI_CAPACITY, budget)
        result.items[0]["text"] += " root=%#x count=%d" % (address, count)
        allocated = int(stats["allocated"])
        if allocated not in (0, 1) or bool(address) != bool(allocated):
            raise ReadFailure("invalid PCI root: inventory allocation unavailable")
        if outcome == 3 and (address or count):
            raise ReadFailure("invalid no-regions PCI inventory")
        matches = 0
        seen = set()
        for index in range(count):
            location = address + index * memory.sizeof("pci_device_t")
            try:
                device = memory.read(location, "pci_device_t", budget)
            except ReadFailure as error:
                result.fail("device %d: %s" % (index, error))
                continue
            identity = tuple(int(device[field]) for field in ("segment", "bus", "device", "function"))
            if identity in seen:
                result.fail("duplicate PCI segment/BDF")
            seen.add(identity)
            if "--device" in selectors and selectors["--device"] != identity:
                continue
            matches += 1
            if identity[2] > 31 or identity[3] > 7:
                result.fail("device %d: invalid BDF" % index)
            text = "source=%#x index=%d device=%04x:%02x:%02x.%x vendor=%04x deviceId=%04x" % (
                location, index, *identity, int(device["vendor_id"]), int(device["device_id"]))
            text += "\n    class=%02x:%02x:%02x revision=%02x command=%#x status=%#x header=%#x multifunction=%d" % tuple(
                int(device[field]) for field in ("class_code", "subclass", "prog_if", "revision_id",
                                                "command", "status", "header_type", "multifunction"))
            text += "\n    subsystem=%04x:%04x capabilitiesPointer=%#x interruptLine=%d interruptPin=%d" % tuple(
                int(device[field]) for field in ("subsystem_vendor_id", "subsystem_id",
                                                "capabilities_pointer", "interrupt_line", "interrupt_pin"))
            bar_count = int(device["bar_count"])
            if not 0 <= bar_count <= 6:
                result.fail("device %d: invalid BAR slot count" % index)
            else:
                for bar_index in range(bar_count):
                    bar = device["bars"][bar_index]
                    text += "\n    BAR%d base=%#x raw=%08x:%08x present=%d io=%d is64=%d prefetchable=%d" % (
                        bar_index, int(bar["base"]), int(bar["raw_high"]), int(bar["raw_low"]),
                        int(bar["present"]), int(bar["is_io"]), int(bar["is_64bit"]), int(bar["prefetchable"]))
            row(result, budget, "pci[%d]" % index, text)
        if selectors and not matches and not result.issues:
            result.fail("PCI selector did not match a captured device")
    except ReadFailure as error:
        result.fail(error)
    return result


pci.selectors = ("--device",)
