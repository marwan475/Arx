"""Virtual-memory RAM inspection. Native contract: VirtualMemoryDebugStats.h."""
from framework.results import ReadFailure, Result
from .memory import descriptor_root, group, number
from .platform import row
from .process import Context, generation

SEMANTICS = "arx.memory.virtual"
LIMIT = 1 << 64
PAGE = 4096
LISTS = ("kernel_free_regions", "kernel_used_regions",
         "user_free_regions", "user_used_regions")


def permissions(value):
    return "".join(letter if value & bit else "-"
                   for bit, letter in ((1, "R"), (2, "W"), (4, "X")))


def canonical(address):
    return 0 <= address < LIMIT and (address < (1 << 47) or address >= LIMIT - (1 << 47))


def linked(memory, pointer, type_name, budget, next_field="next", depth=1):
    if pointer and depth > budget.depth:
        raise ReadFailure("truncated: relationship depth limit")
    seen = set()
    while pointer:
        if pointer in seen:
            raise ReadFailure("cycle in " + type_name)
        if pointer < 0 or pointer % 8 or pointer + memory.sizeof(type_name) > LIMIT:
            raise ReadFailure("invalid or overflowing " + type_name + " address")
        budget.take_entry()
        value = memory.read(pointer, type_name, budget)
        seen.add(pointer)
        yield pointer, value
        pointer = number(value, next_field)


def observed_row(result, name, text):
    # Traversal already charged this entry; preserve it if a later read fails.
    result.items.append({"path": name, "type": "stored observation", "text": text})


def selected_space(memory, stats, selectors, budget, result):
    choices = [key for key in ("--pid", "--cpu", "--slot", "--space") if key in selectors]
    if len(choices) > 1:
        raise ReadFailure("select one of --pid, --cpu, --slot or --space")
    if "--space" in selectors:
        pointer = selectors["--space"]
        label = "explicit typed address space"
    elif "--pid" in selectors:
        process_stats = memory.read(number(stats, "processes"), "ProcessDebugStats", budget)
        context = Context(memory, process_stats, None, budget, result)
        pid = selectors["--pid"]
        process = context.read("process", pid)
        if process is None or number(process, "id") != pid:
            raise ReadFailure("selected process is not allocated")
        identity = generation(process)
        pointer = number(process, "addressSpace")
        label = "PID %d generation %d (active or inactive)" % (pid, identity)
    elif "--cpu" in selectors or "--slot" in selectors:
        source = number(stats, "platform")
        count = int(memory.field(source, "platform_t", "cpu_count", budget))
        base = int(memory.field(source, "platform_t", "cpus", budget))
        if not 1 <= count <= 8 or not base:
            raise ReadFailure("invalid CPU inventory")
        matches = []
        for slot in range(count):
            address = base + slot * memory.sizeof("cpu_info_t")
            logical = int(memory.field(address, "cpu_info_t", "id", budget))
            if selectors.get("--cpu") == logical or selectors.get("--slot") == slot:
                matches.append((slot, logical, address))
        if len(matches) != 1:
            raise ReadFailure("CPU selector is missing or ambiguous")
        slot, logical, address = matches[0]
        pointer = int(memory.field(address, "cpu_info_t", "address_space", budget))
        label = "CPU %d slot %d; configured CPUs=%d" % (logical, slot, count)
    else:
        pointer = number(stats, "kernelSpace")
        label = "kernel address space"
    if not pointer or pointer % 8:
        raise ReadFailure("unavailable or misaligned address-space root")
    value = memory.read(pointer, "virt_addr_space_t", budget)
    if number(value, "type") not in (0, 1):
        raise ReadFailure("invalid address-space type")
    row(result, budget, "selection", "%s; space=%#x" % (label, pointer))
    return pointer, value


def owners(memory, stats, address, budget, result):
    process_stats = memory.read(number(stats, "processes"), "ProcessDebugStats", budget)
    group(result, process_stats["group"], "Process inventory")
    base = number(process_stats, "records")
    if number(process_stats, "capacity") != 64 or not base:
        raise ReadFailure("invalid process identity inventory")
    for slot in range(64):
        pointer = base + slot * memory.sizeof("process_t")
        if not int(memory.field(pointer, "process_t", "allocated", budget)):
            continue
        if int(memory.field(pointer, "process_t", "addressSpace", budget)) == address:
            debug = memory.field(pointer, "process_t", "debug", budget)
            if number(debug, "generation") == 0 or bool(debug["exhausted"]):
                result.fail("process incarnation unavailable")
            row(result, budget, "owner", "PID %d generation %d; process=%#x (owning space reference)"
                % (slot, number(debug, "generation"), pointer))
    source = number(stats, "platform")
    count = int(memory.field(source, "platform_t", "cpu_count", budget))
    base = int(memory.field(source, "platform_t", "cpus", budget))
    if not 1 <= count <= 8 or not base:
        raise ReadFailure("invalid CPU inventory")
    for slot in range(count):
        pointer = base + slot * memory.sizeof("cpu_info_t")
        if int(memory.field(pointer, "cpu_info_t", "address_space", budget)) == address:
            logical = int(memory.field(pointer, "cpu_info_t", "id", budget))
            row(result, budget, "CPU association", "CPU %d slot %d; stored address-space association (may be stale)" % (logical, slot))


def shared_page(memory, pointer, page, budget, result, seen):
    if pointer in seen:
        return
    seen.add(pointer)
    observed_row(result, "shared page", "%#x: resident=%#x offset=%d bytes references=%d dirty=%s shared-accounted=%s (dirty is conservative)" % (
        pointer, number(page, "pointer"), number(page, "offset"), number(page, "references"),
        bool(page["dirty"]), bool(page["shared_accounting"])))


def regions(memory, space, budget, result, show=True, seen_pages=None):
    seen_pages = set() if seen_pages is None else seen_pages
    objects = set()
    resident = set()
    mapped_bytes = 0
    visited = set()
    for name in LISTS:
        previous = 0
        used = "used" in name
        for pointer, region in linked(memory, number(space, name), "virt_region_t", budget):
            if pointer in visited:
                raise ReadFailure("region appears in multiple lists")
            visited.add(pointer)
            start, end, size = (number(region, field) for field in ("start", "end", "size"))
            if not 0 <= start < end <= LIMIT or end - start != size:
                raise ReadFailure("invalid region bounds/size")
            if number(region, "prev") != previous:
                result.fail("region previous link differs during traversal")
            previous = pointer
            if not bool(region["allocated"]):
                result.fail("unallocated region linked in address space")
            region_type = number(region, "type")
            if region_type not in (0, 1):
                result.fail("unknown region type")
            type_name = {0: "kernel", 1: "user"}.get(region_type, "unknown")
            text = "%#x..%#x (%d bytes); type=%s metadata=%#x" % (start, end, size, type_name, pointer)
            if used:
                backing, obj = number(region, "backing"), number(region, "object")
                text += "; %s max=%s; shared mapping=%s; resident=%#x; object=%#x offset=%d; page=%#x; heap=%s" % (
                    permissions(number(region, "protection")), permissions(number(region, "maximum_protection")),
                    bool(region["shared_mapping"]), backing, obj, number(region, "backing_offset"),
                    number(region, "shared"), bool(region["heap_page"]))
                if backing:
                    resident.add(backing)
                    mapped_bytes += size
                if obj:
                    objects.add(obj)
                if show:
                    observed_row(result, name, text)
                shared = number(region, "shared")
                if shared and shared not in seen_pages:
                    if budget.depth < 2:
                        raise ReadFailure("truncated: shared-page relationship depth limit")
                    budget.take_entry()
                    page = memory.read(shared, "virt_shared_page_t", budget)
                    shared_page(memory, shared, page, budget, result, seen_pages)
                for block_address, block in linked(memory, number(region, "blocks"), "virt_backing_block_t", budget, depth=2):
                    if show:
                        observed_row(result, "owned block", "PMM allocation=%#x; link=%#x; rounded size belongs to PMM"
                                     % (number(block, "pointer"), block_address))
            elif show:
                observed_row(result, name, text + "; free reservation metadata")
    if show:
        row(result, budget, "resident observation", "%d mapped region bytes; %d unique resident base addresses; selected lists only"
            % (mapped_bytes, len(resident)))
    return objects


def backing(memory, address, budget, result, seen_pages):
    budget.take_entry()
    value = memory.read(address, "virt_backing_object_t", budget)
    observed_row(result, "backing", "%#x: size=%d bytes; references=%d; page-region mappings=%d; writable mappings=%d; file=%s volatile=%s; operations=%#x context=%#x (opaque provider identity); lock=%d" % (
        address, number(value, "size"), number(value, "references"), number(value, "mappings"),
        number(value, "writable_mappings"), bool(value["file"]), bool(value["volatile_storage"]),
        number(value, "operations"), number(value, "context"), number(value, "lock")))
    if number(value, "lock"):
        result.fail("backing lock held; fields may be transitional")
    for pointer, page in linked(memory, number(value, "pages"), "virt_shared_page_t", budget, depth=2):
        if pointer in seen_pages:
            observed_row(result, "shared page", "%#x: already shown; object edge=%#x" % (pointer, address))
            continue
        shared_page(memory, pointer, page, budget, result, seen_pages)
    return value


def table_entry(memory, stats, physical, budget):
    if not number(stats, "hhdmPresent"):
        raise ReadFailure("unavailable: no HHDM access path")
    virtual = physical + number(stats, "hhdmOffset")
    if virtual + 8 > LIMIT or not canonical(virtual) or not canonical(virtual + 7):
        raise ReadFailure("invalid or overflowing HHDM table address")
    if not number(stats, "hhdmStart") <= virtual < virtual + 8 <= number(stats, "hhdmEnd"):
        raise ReadFailure("page-table entry outside retained HHDM bounds")
    return int.from_bytes(memory.read_bytes(virtual, 8, budget), "little")


def translate_one(memory, stats, root, va, budget, result):
    if (number(stats, "levels"), number(stats, "virtualBits"), number(stats, "physicalBits")) != (4, 48, 52):
        raise ReadFailure("unsupported paging format (requires repository x86_64 four-level model)")
    if not canonical(va):
        raise ReadFailure("noncanonical 48-bit virtual address")
    if root <= 0 or root >= 1 << 52 or root % PAGE:
        raise ReadFailure("invalid physical page-table root")
    writable, user, nx = True, True, False
    table = root
    seen = set()
    mask = ((1 << 52) - 1) & -PAGE
    for depth, (level, shift) in enumerate((("PML4", 39), ("PDPT", 30), ("PD", 21), ("PT", 12))):
        if depth >= budget.depth:
            raise ReadFailure("truncated: translation depth limit")
        if table in seen:
            raise ReadFailure("repeated page-table link")
        seen.add(table)
        index = (va >> shift) & 511
        budget.take_entry()
        entry = table_entry(memory, stats, table + index * 8, budget)
        observed_row(result, level, "table PA=%#x index=%d entry=%#018x; present=%s RW=%s US=%s NX=%s" % (
            table, index, entry, bool(entry & 1), bool(entry & 2), bool(entry & 4), bool(entry >> 63)))
        if not entry & 1:
            row(result, budget, "translation", "VA=%#x: unmapped at %s" % (va, level))
            return PAGE - (va & (PAGE - 1))
        writable = writable and bool(entry & 2)
        user = user and bool(entry & 4)
        nx = nx or bool(entry >> 63)
        huge = bool(entry & 128)
        if level == "PML4" and huge:
            raise ReadFailure("invalid page-size bit at PML4")
        leaf = level == "PT" or huge
        if leaf:
            page_size = 1 << shift
            # Bit 12 is PAT on large leaves; bits 13..shift-1 must be zero.
            if level != "PT" and entry & (((1 << shift) - 1) & ~8191):
                raise ReadFailure("invalid large-page physical alignment/reserved bits")
            physical = (entry & mask & -page_size) + (va & (page_size - 1))
            row(result, budget, "translation", "VA=%#x -> PA=%#x; page=%d bytes offset=%d; page-table permissions=%s user=%s; RAM tables, CPU controls/TLB not observed" % (
                va, physical, page_size, va & (page_size - 1), "R" + ("W" if writable else "-") + ("-" if nx else "X"), user))
            return page_size - (va & (page_size - 1))
        table = entry & mask
        if not table:
            raise ReadFailure("invalid zero intermediate table")


def gather(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    stats = descriptor_root(memory, descriptor, "arx_vmm_debug_stats_t", 0, budget)
    result = Result(source=number(descriptor, "root"), type_name="virtual memory",
                    count_semantics="bounded reachable observations; no global object catalogue or reference census",
                    units="ranges, offsets and sizes are bytes; permissions are labeled")
    try:
        group(result, stats["group"], "VMM mutation")
        for field in ("resourceGroup", "lifecycleGroup"):
            pointer = number(stats, field)
            if pointer:
                group(result, memory.read(pointer, "arx_debug_group_t", budget), field)
        if number(stats, "ready") != 1:
            raise ReadFailure("unavailable: VMM initialization incomplete")
        if name != "translate" and ("--va" in selectors or "--size" in selectors):
            raise ReadFailure("--va/--size require memory translate")
        if name != "backing" and "--value" in selectors:
            raise ReadFailure("numeric object selection requires memory backing")
        if name == "writeback":
            if selectors:
                raise ReadFailure("memory writeback does not accept space selectors")
            if number(stats, "retryActive"):
                result.fail("writeback retry active: detached objects may not be reachable")
            seen = set()
            for address, _ in linked(memory, number(stats, "pendingHead"), "virt_backing_object_t", budget, "pending_next"):
                observed_row(result, "pending writeback", "%#x: retained for retry after last mapping" % address)
                backing(memory, address, budget, result, seen)
            if not result.items:
                row(result, budget, "pending writeback", "queue empty at this stop")
            return result
        if name == "backing" and "--value" in selectors:
            if len(selectors) != 1:
                raise ReadFailure("explicit backing address cannot select an address space")
            address = selectors["--value"]
            if not address or address % 8:
                raise ReadFailure("invalid typed backing-object address")
            backing(memory, address, budget, result, set())
            return result
        address, space = selected_space(memory, stats, selectors, budget, result)
        for field in ("lock", "operation_lock"):
            if number(space, field):
                result.fail("address-space %s held; snapshot may be transitional" % field)
        if name == "space":
            row(result, budget, "space", "type=%s PT=%#x references=%d; lock=%d operation lock=%d" % (
                "kernel" if number(space, "type") == 0 else "user", number(space, "pt"),
                number(space, "references"), number(space, "lock"), number(space, "operation_lock")))
            for kind in ("kernel", "user"):
                pool = space[kind + "_region_metadata_pool"]
                row(result, budget, kind + " metadata", "records=%d; pool chunks=%#x element=%d bytes capacity/chunk=%d; roots: free=%#x used=%#x" % (
                    number(space, kind + "_regions_count"), number(pool, "chunks"), number(pool, "element_size"), number(pool, "elements_per_chunk"),
                    number(space, kind + "_free_regions"), number(space, kind + "_used_regions")))
            shared = int(memory.read(number(stats, "sharedResidentBytes"), "uint64_t", budget))
            row(result, budget, "shared accounting", "%d bytes (maintained unique shared pages, not process RSS)" % shared)
            owners(memory, stats, address, budget, result)
        elif name == "regions":
            regions(memory, space, budget, result)
        elif name == "backing":
            seen = set()
            for obj in sorted(regions(memory, space, budget, result, show=False, seen_pages=seen)):
                backing(memory, obj, budget, result, seen)
        elif name == "translate":
            if "--va" not in selectors:
                raise ReadFailure("memory translate requires --va numeric-address")
            va = selectors["--va"]
            size = selectors.get("--size", 1)
            if va + size > LIMIT or not canonical(va) or not canonical(va + size - 1):
                raise ReadFailure("invalid/noncanonical translation range")
            if va < (1 << 47) and va + size > (1 << 47):
                raise ReadFailure("translation range crosses canonical address hole")
            end = va + size
            while va < end:
                va += translate_one(memory, stats, number(space, "pt"), va, budget, result)
        else:
            raise ReadFailure("unsupported virtual-memory view")
    except (ReadFailure, ValueError, OverflowError) as error:
        result.fail(error)
    return result


gather.selectors = ("--pid", "--cpu", "--slot", "--space", "--va", "--size", "--value")
