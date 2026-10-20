"""Bounded allocator views. Layout/lifetime contracts live in stats headers."""
from framework.results import ReadFailure, Result
from .platform import row

PMM_SEMANTICS = "arx.memory.pmm"
HEAP_SEMANTICS = "arx.memory.heap"
DMA_SEMANTICS = "arx.memory.dma"
NODE_CAPACITY = 8
CPU_CAPACITY = 8
MAX_ORDER = 10
PAGE_BYTES = 4096
SLAB_BYTES = 131072
CLASSES = tuple(16 << index for index in range(14))
ADDRESS_LIMIT = 1 << 64


def checked_range(address, size):
    if address <= 0 or size < 0 or address + size > ADDRESS_LIMIT:
        raise ReadFailure("invalid or overflowing address range")
    return address + size


def number(value, field):
    return int(value[field])


def group(result, value, context):
    writers = number(value, "activeWriters")
    if writers:
        result.fail("%s: operation interrupted (%d active writers); values may be transitional"
                    % (context, writers))


def allocation_text(stats, unit="blocks"):
    text = ("attempts=%d successful=%d failed=%d freed=%d; live=%d " + unit
            + ", %d rounded bytes; successful totals=%d requested bytes, %d rounded bytes")
    return (text % tuple(number(stats, name) for name in
                    ("attempts", "successes", "failures", "frees", "liveBlocks",
                     "liveRoundedBytes", "requestedBytes", "roundedBytes")))


def descriptor_root(memory, descriptor, type_name, capacity, budget):
    if number(descriptor, "version") != 1:
        raise ReadFailure("unsupported allocator view version")
    if number(descriptor, "rootSize") != memory.sizeof(type_name):
        raise ReadFailure("mismatched allocator root size")
    if number(descriptor, "count") != 0 or number(descriptor, "capacity") != capacity:
        raise ReadFailure("invalid allocator descriptor bounds")
    return memory.read(number(descriptor, "root"), type_name, budget)


def nodes(memory, stats, selectors, budget):
    if number(stats, "capacity") != NODE_CAPACITY or number(stats, "ready") != 1:
        raise ReadFailure("allocator unavailable: initialization incomplete or invalid capacity")
    source = number(stats, "source")
    if not source:
        raise ReadFailure("allocator unavailable: no platform root")
    count = int(memory.field(source, "platform_t", "numa_node_count", budget))
    if not 1 <= count <= NODE_CAPACITY:
        raise ReadFailure("invalid NUMA node count")
    base, _ = memory.field_address(source, "platform_t", "numa_nodes")
    stride = memory.sizeof("numa_node_t")
    selected = selectors.get("--node")
    if selected is not None and selected >= count:
        raise ReadFailure("node index outside configured inventory")
    associations = {index: [] for index in range(count)}
    cpu_count = int(memory.field(source, "platform_t", "cpu_count", budget))
    cpu_base = int(memory.field(source, "platform_t", "cpus", budget))
    if not 1 <= cpu_count <= CPU_CAPACITY or not cpu_base:
        raise ReadFailure("invalid CPU association inventory")
    matched = None
    for slot in range(cpu_count):
        location = cpu_base + slot * memory.sizeof("cpu_info_t")
        logical = int(memory.field(location, "cpu_info_t", "id", budget))
        node_pointer = int(memory.field(location, "cpu_info_t", "numa_node", budget))
        offset = node_pointer - base
        if offset < 0 or offset % stride or offset // stride >= count:
            raise ReadFailure("CPU slot %d references an invalid NUMA node" % slot)
        index = offset // stride
        associations[index].append("%d (slot %d)" % (logical, slot))
        if selectors.get("--cpu") == logical or selectors.get("--slot") == slot:
            if matched is not None:
                raise ReadFailure("CPU selection is ambiguous")
            matched = index
    if "--cpu" in selectors or "--slot" in selectors:
        if matched is None:
            raise ReadFailure("CPU selector did not match a configured CPU")
        selected = matched
    return [(index, base + index * stride, associations[index])
            for index in range(count) if selected is None or selected == index]


def zone_regions(zone):
    count = number(zone, "region_count")
    if not 0 <= count <= 128:
        raise ReadFailure("invalid zone region count")
    regions = []
    for index in range(count):
        region = zone["regions"][index]
        start, end = number(region, "start_pfn"), number(region, "end_pfn")
        if end < start or end > ADDRESS_LIMIT // PAGE_BYTES:
            raise ReadFailure("invalid physical region bounds")
        if number(region, "page_count") != end - start:
            raise ReadFailure("physical region page count disagrees with bounds")
        if start != end and any(start < old_end and end > old_start
                                for old_start, old_end in regions):
            raise ReadFailure("overlapping owned physical regions")
        regions.append((start, end))
    return regions


def owns(regions, pfn, pages=1):
    return any(start <= pfn < end and pages <= end - pfn for start, end in regions)


def free_blocks(memory, zone, regions, capacity, order, budget, result):
    base = number(zone, "buddy_metadata")
    stride = memory.sizeof("page_t")
    checked_range(base, capacity * stride)
    pointer = int(zone["buddy_free_lists"][order]["head"])
    previous = 0
    seen = set()
    maximum = sum(end - start for start, end in regions) // (1 << order)
    while pointer:
        if pointer in seen or len(seen) >= maximum:
            raise ReadFailure("buddy list cycle or more blocks than owned pages allow")
        offset = pointer - base
        if offset < 0 or offset % stride or offset // stride >= capacity:
            raise ReadFailure("buddy link outside the metadata allocation")
        budget.take_entry()
        block = memory.read(pointer, "page_t", budget)
        pfn = number(block, "pfn")
        if pfn != offset // stride or pfn % (1 << order) or not owns(regions, pfn, 1 << order):
            raise ReadFailure("buddy block does not belong to this zone/order")
        if number(block, "flags") != 1 or number(block, "order") != order:
            raise ReadFailure("buddy list head flags/order disagree with its list")
        if number(block, "prev") != previous:
            raise ReadFailure("buddy previous link disagrees with traversal")
        seen.add(pointer)
        yield pointer, pfn
        previous, pointer = pointer, number(block, "next")
    expected = int(zone["debugStats"]["freeBlocks"][order])
    if len(seen) != expected:
        result.fail("order %d: observed %d free blocks; maintained count is %d"
                    % (order, len(seen), expected))


def pmm(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    stats = descriptor_root(memory, descriptor, "arx_pmm_debug_stats_t", NODE_CAPACITY, budget)
    result = Result(source=number(descriptor, "root"), type_name="PMM / NUMA",
                    count_semantics="unique NUMA nodes and owned zones; bounded stored observations",
                    units="pages, blocks and bytes are labeled; PFNs are page numbers")
    try:
        if name != "blocks" and "--order" in selectors:
            raise ReadFailure("--order is only supported by pmm blocks")
        selected_pfn = selectors.get("--value")
        if (name == "pfn") != (selected_pfn is not None):
            raise ReadFailure("pmm pfn requires one numeric PFN")
        capacity = number(stats, "metadataCapacity")
        if not 0 < capacity <= ADDRESS_LIMIT // memory.sizeof("page_t"):
            raise ReadFailure("invalid absolute-PFN metadata capacity")
        if selected_pfn is not None and selected_pfn >= capacity:
            raise ReadFailure("PFN outside the metadata allocation")
        row(result, budget, "PMM", "boot metadata reservation=%d bytes; metadata covers PFNs [0,%d)"
            % (number(stats, "metadataReservedBytes"), capacity))
        found = False
        owned_ranges = []
        for index, location, cpus in nodes(memory, stats, selectors, budget):
            domain = int(memory.field(location, "numa_node_t", "proximity_domain", budget))
            row(result, budget, "Node %d" % index,
                "domain=%d; CPUs=%s" % (domain, ", ".join(cpus) or "none"))
            for zone_name, field in (("normal", "zone"), ("dma", "dma_zone")):
                zone = memory.field(location, "numa_node_t", field, budget)
                regions = zone_regions(zone)
                if any(start < other_end and end > other_start
                       for start, end in regions for other_start, other_end in owned_ranges):
                    raise ReadFailure("selected zones have overlapping physical ownership")
                owned_ranges.extend(regions)
                if "--zone" in selectors and selectors["--zone"] != zone_name:
                    continue
                context = "Node %d / %s" % (index, zone_name)
                group(result, zone["debugStats"]["group"], context)
                free_counts = [int(zone["debugStats"]["freeBlocks"][order])
                               for order in range(MAX_ORDER + 1)]
                free_pages = number(zone, "free_pages")
                total = number(zone, "total_pages")
                used = number(zone, "used_pages")
                if (sum(end - start for start, end in regions) != total
                        or free_pages + used != total
                        or sum(count << order for order, count in enumerate(free_counts)) != free_pages):
                    result.fail(context + ": page totals and free-block counts disagree")
                largest = max((order for order, count in enumerate(free_counts) if count), default=-1)
                row(result, budget, context,
                    "%d total pages; %d free; %d used; largest free block=%d bytes (order %s)"
                    % (total, free_pages, used, PAGE_BYTES << largest if largest >= 0 else 0,
                       str(largest) if largest >= 0 else "none"))
                if name in ("zones", "orders"):
                    row(result, budget, context + " accounting", allocation_text(zone["debugStats"]["operations"]))
                if name == "zones":
                    row(result, budget, context + " address model",
                        "zone=%#x; observed lock=%d; broad PFN envelope=[%d,%d); "
                        "HHDM present=%s offset=%#x; metadata=%#x; stored memory=%d bytes"
                        % (memory.field_address(location, "numa_node_t", field)[0], number(zone, "lock"),
                           number(zone, "min_pfn"), number(zone, "max_pfn"),
                           bool(zone["hhdm_present"]), number(zone, "hhdm_offset"),
                           number(zone, "buddy_metadata"), number(zone, "total_memory")))
                    for start, end in regions:
                        row(result, budget, context + " owned range",
                            "physical=[%#x,%#x); PFNs=[%d,%d); %d pages"
                            % (start * PAGE_BYTES, end * PAGE_BYTES, start, end, end - start))
                if name == "orders":
                    for order, count in enumerate(free_counts):
                        row(result, budget, context + " order %d" % order,
                            "%d free blocks; %d pages; %d bytes per block"
                            % (count, count << order, PAGE_BYTES << order))
                if name == "blocks":
                    orders = [selectors["--order"]] if "--order" in selectors else range(MAX_ORDER + 1)
                    for order in orders:
                        row(result, budget, context + " order %d" % order,
                            "%d maintained free blocks; %d bytes per block" % (free_counts[order], PAGE_BYTES << order))
                        for pointer, pfn in free_blocks(memory, zone, regions, capacity, order, budget, result):
                            result.items.append({"path": "Free block", "type": "validated buddy head",
                                                 "text": "PFN=%d physical=%#x metadata=%#x" % (pfn, pfn * PAGE_BYTES, pointer)})
                if name == "pfn" and owns(regions, selected_pfn):
                    found = True
                    pointer = number(zone, "buddy_metadata") + selected_pfn * memory.sizeof("page_t")
                    checked_range(number(zone, "buddy_metadata"), capacity * memory.sizeof("page_t"))
                    page = memory.read(pointer, "page_t", budget)
                    order = number(page, "order")
                    physical = selected_pfn * PAGE_BYTES
                    cpu_address = physical + (number(zone, "hhdm_offset") if bool(zone["hhdm_present"]) else 0)
                    if cpu_address >= ADDRESS_LIMIT:
                        raise ReadFailure("HHDM interpretation overflows the address model")
                    row(result, budget, "Selected PFN",
                        "PFN=%d physical=%#x interpreted CPU address=%#x; metadata=%#x; owned by %s; raw flags=%d order=%d stored PFN=%d"
                        % (selected_pfn, physical, cpu_address, pointer, context, number(page, "flags"), order, number(page, "pfn")))
                    validated = False
                    if number(page, "flags") == 1 and order <= MAX_ORDER:
                        for _, pfn in free_blocks(memory, zone, regions, capacity, order, budget, result):
                            if pfn == selected_pfn:
                                validated = True
                    row(result, budget, "Interpretation",
                        "validated free block head" if validated else
                        "allocation ownership unknown; interior page metadata may be stale")
        if name == "pfn" and not found:
            row(result, budget, "Selected PFN", "not in any selected zone's owned ranges; metadata alone cannot establish ownership")
    except ReadFailure as error:
        result.fail(error)
    return result


pmm.selectors = ("--node", "--cpu", "--slot", "--zone", "--order", "--value")


def slab_walk(memory, head, ranges, budget):
    seen = set()
    pointer = int(head)
    previous = 0
    while pointer:
        if pointer in seen:
            raise ReadFailure("slab list cycle or duplicate slab")
        stride = memory.sizeof("slab_t")
        if not any(start <= pointer and pointer + stride <= end
                   and (pointer - start) % stride == 0 for start, end in ranges):
            raise ReadFailure("slab pointer outside its cache metadata pool")
        budget.take_entry()
        slab = memory.read(pointer, "slab_t", budget)
        if number(slab, "prev") != previous:
            raise ReadFailure("slab previous link disagrees with traversal")
        seen.add(pointer)
        yield pointer, slab
        previous, pointer = pointer, number(slab, "next")


def pool_detail(memory, pool, budget, result, emit=True):
    pointer = number(pool, "chunks")
    previous = 0
    seen = set()
    size = number(pool, "element_size")
    count = number(pool, "elements_per_chunk")
    if size != memory.sizeof("slab_t") or not 0 < count <= 65536:
        raise ReadFailure("invalid slab metadata pool shape")
    ranges = []
    if emit:
        row(result, budget, "Metadata pool",
            "element=%d bytes; %d elements per chunk; reusable free-list head=%#x"
            % (size, count, number(pool, "free_list")))
    while pointer:
        if pointer in seen:
            raise ReadFailure("metadata chunk list cycle")
        seen.add(pointer)
        budget.take_entry()
        chunk = memory.read(pointer, "metadata_chunk_t", budget)
        if number(chunk, "prev") != previous or number(chunk, "size") != size * count:
            raise ReadFailure("invalid metadata chunk bounds/links")
        data = number(chunk, "data")
        end = checked_range(data, size * count)
        if any(data < old_end and end > old_start for old_start, old_end in ranges):
            raise ReadFailure("metadata chunk data ranges overlap")
        ranges.append((data, end))
        if emit:
            result.items.append({"path": "Metadata chunk", "type": "slab metadata",
                                 "text": "record=%#x data=[%#x,%#x); %d bytes" % (pointer, data, end, size * count)})
        previous, pointer = pointer, number(chunk, "next")
    return ranges


def heap(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    stats = descriptor_root(memory, descriptor, "arx_heap_debug_stats_t", NODE_CAPACITY, budget)
    result = Result(source=number(descriptor, "root"), type_name="NUMA heap",
                    count_semantics="maintained cache totals; slab/address searches are bounded",
                    units="object sizes and backing sizes in bytes; object counts labeled")
    try:
        address = selectors.get("--value")
        if (name == "address") != (address is not None):
            raise ReadFailure("heap address requires one numeric address")
        if "--check-bitmap" in selectors and name not in ("slabs", "address"):
            raise ReadFailure("bitmap checks require heap slabs or heap address")
        if "--class" in selectors and selectors["--class"] not in CLASSES:
            raise ReadFailure("heap class must be an object size from 16 through 131072 bytes")
        row(result, budget, "Heap calls",
            "%d allocations, %d failures, %d free calls; live requested bytes unavailable"
            % (number(stats, "allocationCalls"), number(stats, "allocationFailures"), number(stats, "freeCalls")))
        found = False
        seen_slabs = set()
        for index, location, cpus in nodes(memory, stats, selectors, budget):
            heap_address, _ = memory.field_address(location, "numa_node_t", "heap")
            heap_value = memory.read(heap_address, "kernel_heap_t", budget)
            group(result, heap_value["debugGroup"], "Node %d heap" % index)
            row(result, budget, "Node %d heap" % index,
                "address=%#x; observed lock=%d; CPUs=%s"
                % (heap_address, number(heap_value, "lock"), ", ".join(cpus) or "none"))
            for class_index, size in enumerate(CLASSES):
                if "--class" in selectors and size != selectors["--class"]:
                    continue
                cache = heap_value["caches"][class_index]
                if number(cache, "object_size") != size:
                    raise ReadFailure("cache size disagrees with fixed size-class index")
                debug = cache["debugStats"]
                if name != "address":
                    row(result, budget, "Class %d bytes" % size,
                        allocation_text(debug["operations"], "objects")
                        + "\n    initialized slabs=%d; backing=%d payload bytes + %d bitmap bytes; "
                        "failed initializations=%d (%d retained bytes)"
                        % tuple(number(debug, field) for field in ("slabsInitialized", "payloadBackingBytes",
                            "bitmapBackingBytes", "slabInitFailures", "retainedInitFailureBytes")))
                if name == "caches":
                    row(result, budget, "Class %d native roots" % size,
                        "partial=%#x full=%#x empty=%#x; metadata chunks=%#x reusable=%#x; "
                        "element=%d bytes, %d per chunk; list names describe availability, not occupancy"
                        % (number(cache, "partial_slabs"), number(cache, "full_slabs"), number(cache, "empty_slabs"),
                           number(cache["slab_metadata_pool"], "chunks"), number(cache["slab_metadata_pool"], "free_list"),
                           number(cache["slab_metadata_pool"], "element_size"), number(cache["slab_metadata_pool"], "elements_per_chunk")))
                    continue
                if not any(number(cache, field) for field in ("partial_slabs", "full_slabs", "empty_slabs")) and name == "address":
                    continue
                ranges = pool_detail(memory, cache["slab_metadata_pool"], budget, result, emit=name == "slabs")
                list_totals = {}
                for native in ("partial_slabs", "full_slabs", "empty_slabs"):
                    slabs = objects = free_objects = 0
                    for pointer, slab in slab_walk(memory, cache[native], ranges, budget):
                        if pointer in seen_slabs:
                            raise ReadFailure("slab appears in more than one native list/cache")
                        seen_slabs.add(pointer)
                        total, free = number(slab, "total_objects"), number(slab, "free_objects")
                        if (number(slab, "object_size") != size or total != SLAB_BYTES // size
                                or free > total or not int(slab["allocated"])):
                            raise ReadFailure("invalid slab size/count/allocation metadata")
                        start = number(slab, "objects_array")
                        end = checked_range(start, total * size)
                        slabs += 1
                        objects += total
                        free_objects += free
                        if name == "address" and not start <= address < end:
                            continue
                        found = True
                        occupancy = "free" if free == total else "exhausted" if free == 0 else "partially occupied"
                        text = "node=%d class=%d bytes; native=%s; occupancy=%s; %d/%d objects free\n    " % (
                            index, size, native, occupancy, free, total)
                        text += "slab=%#x objects=[%#x,%#x); bitmap=%#x" % (pointer, start, end, number(slab, "object_bitmap"))
                        if name == "address":
                            slot, offset = divmod(address - start, size)
                            bitmap = memory.read_bytes(number(slab, "object_bitmap") + slot // 8, 1, budget)
                            text += "\n    slot=%d offset=%d bytes; %s; allocated=%s" % (
                                slot, offset, "object start" if offset == 0 else "interior address", bool(bitmap[0] & (1 << (slot % 8))))
                        if selectors.get("--check-bitmap"):
                            bitmap = memory.read_bytes(number(slab, "object_bitmap"), (total + 7) // 8, budget)
                            allocated = sum((bitmap[bit // 8] >> (bit % 8)) & 1 for bit in range(total))
                            text += "; bitmap allocated=%d objects" % allocated
                            if allocated != total - free:
                                result.fail("slab %#x: bitmap and free-object count disagree" % pointer)
                        result.items.append({"path": "Slab" if name == "slabs" else "Selected address", "type": "stored slab observation", "text": text})
                    list_totals[native] = (slabs, objects, free_objects)
                    if name == "slabs":
                        row(result, budget, "Class %d / %s" % (size, native),
                            "%d slabs; %d objects; %d free objects" % list_totals[native])
                if name == "slabs":
                    live_objects = sum(total - free for _, total, free in list_totals.values())
                    if live_objects != number(debug["operations"], "liveBlocks"):
                        result.fail("class %d: list occupancy and maintained live objects disagree" % size)
        if name == "address" and not found:
            row(result, budget, "Selected address", "not found in selected slab lists; no object provenance is available")
    except ReadFailure as error:
        result.fail(error)
    return result


heap.selectors = ("--node", "--cpu", "--slot", "--class", "--value", "--check-bitmap")


DMA_PHASES = {0: "preparing", 1: "live", 2: "rollback retained", 3: "unmapping",
              4: "unmapped; backing release pending", 5: "backing released; unlink pending"}
DMA_STATUSES = {0: "success", 1: "invalid argument", 2: "unsupported", 3: "no memory",
                4: "mapping failed", 5: "invalid allocation", 6: "busy"}


def range_valid(address, size, mask, alignment, boundary):
    if not mask or mask & (mask + 1) or not alignment or alignment & (alignment - 1) or boundary & (boundary - 1):
        return False
    if size <= 0 or address + size > ADDRESS_LIMIT or address & (alignment - 1):
        return False
    end = address + size - 1
    return end <= mask and (not boundary or address // boundary == end // boundary)


def dma(name, memory, descriptor, budget, selectors=None):
    stats = descriptor_root(memory, descriptor, "DmaDebugStats", 0, budget)
    result = Result(source=number(descriptor, "root"), type_name="Coherent DMA",
                    count_semantics="tracked records include failed-allocation cleanup handles; newest first",
                    units="all sizes in bytes; CPU, physical and device addresses are distinct")
    try:
        group(result, stats["group"], "DMA")
        source = number(stats, "source")
        if not source:
            raise ReadFailure("DMA manager unavailable")
        ops = memory.field(source, "DmaManager", "Operations", budget)
        # Compare stored function identities only; never invoke them or follow
        # backend context, PCI pointers, mapping cookies, or buffer addresses.
        names = ("context", "query", "map", "unmap", "writeBarrier", "readBarrier")
        backend = {0: "unavailable backend", 1: "platform direct-address backend",
                   2: "custom backend"}.get(number(stats, "backendKind"), "unknown backend")
        row(result, budget, "Backend", backend + "; " + " ".join("%s=%#x" % (field, number(ops, field)) for field in names))
        count = int(memory.field(source, "DmaManager", "AllocationCount", budget))
        pointer = int(memory.field(source, "DmaManager", "Allocations", budget))
        if count < 0 or count >= ADDRESS_LIMIT:
            raise ReadFailure("invalid tracked DMA record count")
        row(result, budget, "Retained DMA",
            "%d tracked records: %d successful-origin, %d cleanup-only; "
            "%d requested bytes, %d mapped bytes, %d reserved backing bytes"
            % (count, number(stats, "successfulRecords"), number(stats, "cleanupRecords"),
               number(stats, "retainedRequestedBytes"), number(stats, "retainedMappedBytes"), number(stats, "retainedReservedBytes")))
        row(result, budget, "DMA operations",
            "%d allocation calls; %d free calls; %d retained rollbacks; %d cleanup completions"
            % tuple(number(stats, field) for field in ("allocationCalls", "freeCalls", "rollbackRetained", "cleanupCompletions")))
        for field, label in (("allocationOutcomes", "Allocation outcomes"), ("freeOutcomes", "Free outcomes")):
            row(result, budget, label, "; ".join("%s=%d" % (DMA_STATUSES[index], int(stats[field][index])) for index in range(7)))
        seen, ids = set(), set()
        requested = mapped = reserved = successes = cleanups = 0
        while pointer:
            if pointer in seen:
                raise ReadFailure("DMA allocation list cycle")
            budget.take_entry()
            record = memory.read(pointer, "DmaManager::AllocationRecord", budget)
            seen.add(pointer)
            public, debug, device, mapping = (record[field] for field in ("allocation", "debug", "device", "mapping"))
            identity = number(public, "id")
            if not identity or identity in ids:
                raise ReadFailure("invalid or duplicate DMA allocation ID")
            ids.add(identity)
            phase = number(debug, "phase")
            if phase not in DMA_PHASES:
                result.fail("unknown DMA cleanup phase: %d" % phase)
            size, reserved_size = number(record, "mappedSize"), number(record, "reservedSize")
            alignment, boundary = number(debug, "effectiveAlignment"), number(debug, "effectiveBoundary")
            device_address = number(mapping["address"], "value")
            cleanup_only = bool(debug["cleanupOnly"])
            cleanups += int(cleanup_only)
            successes += int(not cleanup_only)
            requested += size
            mapped += size if bool(debug["mappingHeld"]) else 0
            reserved += reserved_size if bool(debug["backingHeld"]) else 0
            valid = range_valid(device_address, size, number(device, "addressMask"), alignment, boundary)
            if size <= 0 or reserved_size < size or reserved_size & (reserved_size - 1):
                result.fail("DMA record has invalid requested/reserved sizes")
            text = "id=%d; %s; phase=%s; releasing=%s\n    " % (
                identity, "cleanup-only failed allocation" if cleanup_only else "successful allocation",
                DMA_PHASES.get(phase, "unknown"), bool(record["releasing"]))
            text += "CPU=%#x backing=%#x physical=%#x device=%#x; cookie=%#x\n    " % (
                number(public, "cpuAddress"), number(record, "backing"), number(record, "physical"),
                device_address, number(mapping, "cookie"))
            text += "requested=%d mapped=%d reserved=%d bytes; last observed backing held=%s mapping held=%s\n    " % (
                size, size if bool(debug["mappingHeld"]) else 0, reserved_size,
                bool(debug["backingHeld"]), bool(debug["mappingHeld"]))
            text += "mask=%#x physical mask=%#x; alignment requested=%d device=%d effective=%d; " % (
                number(device, "addressMask"), number(debug, "physicalMask"), number(debug, "requestedAlignment"),
                number(device, "minimumAlignment"), alignment)
            text += "boundary requested=%d device=%d effective=%d bytes\n    " % (
                number(debug, "requestedBoundary"), number(device, "boundary"), boundary)
            physical = number(record, "physical")
            physical_mask = number(debug, "physicalMask")
            physical_valid = reserved_size > 0 and physical + reserved_size <= ADDRESS_LIMIT and physical + reserved_size - 1 <= physical_mask
            cpu_valid = alignment > 0 and number(record, "backing") % alignment == 0
            text += "stored device range satisfies constraints=%s; CPU alignment valid=%s; physical reservation fits mask=%s\n    " % (
                valid, cpu_valid, physical_valid)
            cleanup_status = number(debug, "lastCleanupStatus")
            cleanup_result = DMA_STATUSES.get(cleanup_status, "unknown")
            if cleanup_status == 0 and phase in (0, 1, 3):
                cleanup_result = "no result recorded"
            text += "last cleanup=%s; PCI identity=%#x" % (
                cleanup_result, number(device, "pciDevice"))
            result.items.append({"path": "DMA allocation", "type": "stored record", "text": text})
            pointer = number(record, "next")
        if (len(seen) != count or successes != number(stats, "successfulRecords")
                or cleanups != number(stats, "cleanupRecords")
                or requested != number(stats, "retainedRequestedBytes")
                or mapped != number(stats, "retainedMappedBytes")
                or reserved != number(stats, "retainedReservedBytes")):
            result.fail("DMA list and maintained record/byte totals disagree")
    except ReadFailure as error:
        result.fail(error)
    return result
