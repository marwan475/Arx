"""IPC and UNIX socket observations. Native contracts: *State.hpp/*DebugStats.hpp."""
from framework import readers
from framework.results import ReadFailure, Result
from .memory import descriptor_root, group, number
from .platform import row
from .process import Context, generation
from .virtual_memory import backing as show_backing

IPC = "arx.ipc"
SOCKETS = "arx.sockets"
LIMIT = 1 << 64
TYPES = {1: "STREAM", 2: "DGRAM", 5: "SEQPACKET"}


def address_text(value, length):
    if length == 0:
        return "unnamed"
    if not 2 <= length <= 110 or number(value, "family") != 1:
        raise ReadFailure("invalid UNIX address family/length")
    data = bytes(int(value["path"][index]) & 255 for index in range(length - 2))
    if not data:
        return "unnamed"
    if data[0] == 0:
        return "abstract " + repr(data[1:])
    if b"\x00" not in data:
        raise ReadFailure("pathname address lacks bounded terminator")
    return "pathname " + repr(data.split(b"\x00", 1)[0])


class Graph:
    def __init__(self, memory, stats, budget, result, selectors):
        self.memory, self.stats, self.budget, self.result = memory, stats, budget, result
        self.selectors = selectors
        self.payload_left = selectors.get("--payload-bytes", 0)
        if not 0 <= self.payload_left <= 4096:
            raise ReadFailure("payload cap outside 0..4096 bytes")
        self.scan_left = selectors.get("--scan", 4096)
        if not 1 <= self.scan_left <= 1048576:
            raise ReadFailure("invalid descriptor scan ceiling")
        self.files, self.owners, self.selected = {}, {}, set()
        self.endpoints, self.packets, self.controls = {}, {}, {}
        self.processes = self.read(number(stats, "processes"), "ProcessDebugStats")
        tasks = number(self.processes, "taskStats")
        task_stats = self.read(tasks, "TaskDebugStats") if tasks else None
        self.context = Context(memory, self.processes, task_stats, budget, result)
        self.tables = {}
        self.matched_owner = False
        if "--fd" in selectors and not any(key in selectors for key in ("--pid", "--tid")):
            raise ReadFailure("--fd requires --pid or --tid")

    def read(self, pointer, type_name):
        if pointer <= 0 or pointer + self.memory.sizeof(type_name) > LIMIT:
            raise ReadFailure("null/overflowing " + type_name + " pointer")
        return self.memory.read(pointer, type_name, self.budget)

    def field(self, pointer, type_name, field):
        if not pointer:
            raise ReadFailure("null " + type_name + " root")
        return self.memory.field(pointer, type_name, field, self.budget)

    def chain(self, pointer, type_name, maximum=128, next_field="next", depth=1):
        if pointer and depth > self.budget.depth:
            raise ReadFailure("truncated: relationship depth")
        seen = set()
        while pointer:
            if pointer in seen:
                raise ReadFailure("cycle in " + type_name)
            if len(seen) >= maximum:
                raise ReadFailure("invalid/excessive " + type_name + " chain count")
            if pointer % 8 or pointer < 0 or pointer + self.memory.sizeof(type_name) > LIMIT:
                raise ReadFailure("invalid " + type_name + " address")
            seen.add(pointer)
            # Common array reader copies exactly one metadata node per step.
            collected = readers.array(self.memory, pointer, type_name, 1, 1, self.budget)
            if collected.issues:
                raise ReadFailure("; ".join(collected.issues))
            value = collected.items[0]["value"]
            yield pointer, value
            pointer = number(value, next_field)

    def file(self, pointer):
        if pointer not in self.files:
            self.budget.take_entry()
            value = self.read(pointer, "file_t")
            if number(value, "refCount") == 0:
                raise ReadFailure("file has no retained references (retiring/partial)")
            if number(value, "ioLock") or number(value, "refLock"):
                self.result.fail("file lock held; file state may be transitional")
            self.files[pointer] = value
        return self.files[pointer]

    def identity(self, pointer):
        file = self.file(pointer)
        return "file=%#x references=%d owners=%s" % (
            pointer, number(file, "refCount"), "; ".join(self.owners.get(pointer, [])) or "none observed; may be in flight/retained")

    def discover_descriptors(self):
        selecting = "--pid" in self.selectors or "--tid" in self.selectors
        for kind in ("process", "task"):
            for slot, value in self.context.inventory(kind):
                self.budget.take_entry()
                pid, process = (slot, value) if kind == "process" else self.context.process_for_task(value)
                if kind == "task" and process is None:
                    continue
                identity = "%s slot=%d id=%d generation=%d" % (kind, slot, number(value, "id"), generation(value))
                table = number(value, "descriptorTable")
                if not table and kind == "task":
                    table = number(process, "descriptorTable")
                    identity += " (process fallback)"
                match = not selecting or (kind == "process" and self.selectors.get("--pid") == slot) or (
                    kind == "task" and self.selectors.get("--tid") == number(value, "id"))
                self.matched_owner |= match
                if table:
                    owners, selected = self.tables.setdefault(table, ([], False))
                    owners.append(identity)
                    self.tables[table] = owners, selected or match
        if selecting and not self.matched_owner:
            raise ReadFailure("selected process/task is not allocated")
        route = number(self.processes, "vfsRoute")
        if not route:
            raise ReadFailure("verified VFS descriptor route unavailable")
        for pointer, (owners, selected) in self.tables.items():
            table = self.read(pointer, "descriptor_table_t")
            count, base = number(table, "count"), number(table, "entries")
            if number(table, "lock"):
                self.result.fail("descriptor table lock held; ownership may be transitional")
            if not 0 <= count <= 1048576 or (count and not base) or not number(table, "references"):
                raise ReadFailure("invalid descriptor table count/storage/references")
            for index in range(count):
                if self.scan_left <= 0:
                    raise ReadFailure("truncated: descriptor scan ceiling; reachability incomplete")
                self.scan_left -= 1
                descriptor = self.read(base + index * self.memory.sizeof("file_descriptor_t"), "file_descriptor_t")
                file_pointer = number(descriptor, "file")
                wanted = selected and self.selectors.get("--fd", index) == index
                if not file_pointer:
                    continue
                self.budget.take_entry()
                if number(descriptor, "operations") != route:
                    if wanted:
                        self.result.fail("FD %d has unverified/custom descriptor route; opaque handle" % index)
                    continue
                edge = "table=%#x fd=%d [%s] (owning descriptor)" % (pointer, index, ", ".join(owners))
                self.owners.setdefault(file_pointer, []).append(edge)
                self.file(file_pointer)
                if wanted:
                    self.selected.add(file_pointer)
        if "--fd" in self.selectors and not self.selected:
            raise ReadFailure("selected FD has no verified open file")

    def catalogue(self, stats):
        if number(stats, "capacity") != 128:
            raise ReadFailure("invalid endpoint physical capacity")
        group(self.result, stats["group"], "socket manager")
        expected = int(self.read(number(stats, "count"), "size_t"))
        if not 0 <= expected <= 128:
            raise ReadFailure("invalid endpoint count")
        head = int(self.read(number(stats, "head"), "uint64_t"))
        for pointer, endpoint in self.chain(head, "socket_endpoint_t"):
            if number(endpoint, "manager") != number(stats, "source"):
                raise ReadFailure("endpoint belongs to a different manager")
            file = self.file(number(endpoint, "file"))
            inode, _ = self.memory.field_address(pointer, "socket_endpoint_t", "inode")
            if (number(file, "operations") != number(stats, "operations") or not number(stats, "operations")
                    or number(file, "privateData") != pointer or number(file, "inode") != inode):
                raise ReadFailure("socket file operations/privateData/inode mismatch")
            if number(endpoint["inode"], "type") != 5:
                raise ReadFailure("socket inode type mismatch")
            if number(endpoint, "type") not in TYPES:
                raise ReadFailure("unsupported UNIX endpoint type")
            self.endpoints[pointer] = endpoint
        if len(self.endpoints) != expected:
            self.result.fail("endpoint catalogue length differs from stored count")

    def messages(self, pointer, endpoint):
        if pointer in self.packets:
            return self.packets[pointer]
        packets = self.packets.setdefault(pointer, [])
        remaining = 0
        for packet_pointer, packet in self.chain(number(endpoint, "head"), "socket_packet_t", depth=2):
            size, offset = number(packet, "size"), number(packet, "offset")
            if not 0 <= offset <= size <= 65536:
                raise ReadFailure("invalid packet size/offset")
            remaining += size - offset
            packets.append((packet_pointer, packet))
        if len(packets) != number(endpoint, "packetCount") or remaining != number(endpoint, "queued"):
            self.result.fail("message chain count/remaining bytes differ from endpoint accounting")
        if number(endpoint, "tail") != (packets[-1][0] if packets else 0):
            self.result.fail("message tail differs from observed FIFO")
        return packets

    def ancillary(self, packet_pointer, packet):
        if packet_pointer in self.controls:
            return self.controls[packet_pointer]
        controls = self.controls.setdefault(packet_pointer, [])
        for pointer, control in self.chain(number(packet, "control"), "socket_ancillary_t", depth=3):
            count = number(control, "count")
            if not 0 <= count <= 253:
                raise ReadFailure("invalid ancillary rights count")
            files = []
            for index in range(count):
                self.budget.take_entry()
                file_pointer = int(control["files"][index])
                self.file(file_pointer)
                files.append(file_pointer)
            controls.append((pointer, control, files))
        return controls

    def payload(self, pointer, length, label):
        amount = min(length, self.payload_left)
        if amount:
            self.payload_left -= amount
            data = self.memory.read_bytes(pointer, amount, self.budget)
            row(self.result, self.budget, label, repr(data) + "; explicit payload, %d/%d bytes" % (amount, length))
        return amount

    def pending(self, pointer, endpoint):
        seen = []
        for child_pointer, child in self.chain(number(endpoint, "pendingHead"), "socket_endpoint_t", next_field="pendingNext", depth=2):
            if child_pointer not in self.endpoints:
                raise ReadFailure("pending accept endpoint absent from catalogue")
            seen.append(child_pointer)
            row(self.result, self.budget, "  pending accept", "endpoint=%#x %s; owning queued file, internalReferences=%d" % (
                child_pointer, self.identity(number(child, "file")), number(child, "internalReferences")))
        if len(seen) != number(endpoint, "pendingCount") or number(endpoint, "pendingTail") != (seen[-1] if seen else 0):
            self.result.fail("pending accept count/tail differs from observed chain")

    def mapping_edges(self, backing):
        seen_spaces = set()
        for slot, process in self.context.inventory("process"):
            space_pointer = number(process, "addressSpace")
            if not space_pointer or space_pointer in seen_spaces:
                continue
            seen_spaces.add(space_pointer)
            space = self.read(space_pointer, "virt_addr_space_t")
            if number(space, "operation_lock"):
                self.result.fail("address-space operation lock held; mapping relations may be transitional")
            # All owning process spaces, not only the selected descriptor owner.
            for field in ("kernel_used_regions", "user_used_regions"):
                for region_pointer, region in self.chain(number(space, field), "virt_region_t", maximum=1048576, depth=2):
                    if number(region, "object") == backing:
                        row(self.result, self.budget, "  mapping", "pid=%d generation=%d space=%#x region=%#x [%#x,%#x) backing=%#x offset=%d shared=%s page=%#x" % (
                            slot, generation(process), space_pointer, region_pointer, number(region, "start"), number(region, "end"),
                            backing, number(region, "backing_offset"), bool(region["shared_mapping"]), number(region, "shared")))


def pipe(graph, pointer, file):
    if int(graph.field(number(file, "inode"), "inode_t", "type")) != 6:
        raise ReadFailure("pipe inode type mismatch")
    memory, budget, result = graph.memory, graph.budget, graph.result
    state = number(file, "privateData")
    inode, _ = memory.field_address(state, "pipe_state_t", "inode")
    if not state or number(file, "inode") != inode:
        raise ReadFailure("pipe privateData/embedded inode mismatch")
    # Never copy pipe_state_t: data[65536] is intentionally skipped.
    value = {name: graph.field(state, "pipe_state_t", name) for name in (
        "reader", "writer", "references", "capacity", "head", "size", "lastReadiness", "pendingReadiness", "debugGroup")}
    group(result, value["debugGroup"], "pipe object")
    capacity, head, size = number(value, "capacity"), number(value, "head"), number(value, "size")
    if not 4096 <= capacity <= 65536 or capacity & (capacity - 1) or not 0 <= head < capacity or not 0 <= size <= capacity:
        raise ReadFailure("invalid pipe ring capacity/head/size")
    row(result, budget, "pipe=%#x" % state, "%s; endpointObjects=%d capacity=%d head=%d used=%d bytes lastReadiness=%s pendingReadiness=%s" % (
        graph.identity(pointer), number(value, "references"), capacity, head, size,
        [int(value["lastReadiness"][i]) for i in range(2)], [int(value["pendingReadiness"][i]) for i in range(2)]))
    if number(value, "references") > 2:
        result.fail("invalid pipe endpoint-object reference count")
    for name in ("reader", "writer"):
        endpoint = number(value, name)
        if endpoint:
            linked = graph.file(endpoint)
            if number(linked, "operations") != number(file, "operations") or number(linked, "privateData") != state:
                result.fail("pipe %s file link differs from operation/state identity" % name)
            row(result, budget, "  " + name, graph.identity(endpoint) + "; weak endpoint link")
        else:
            row(result, budget, "  " + name, "closed (null endpoint)")
    metadata_address, _ = memory.field_address(state, "pipe_state_t", "packet")
    # At most 16KiB of packet metadata, independent of payload detail.
    metadata = memory.read_bytes(metadata_address, (capacity + 3) // 4, budget) if size else b""
    start, previous = 0, None
    for logical in range(size):
        physical = (head + logical) % capacity
        flags = metadata[physical // 4] >> ((physical % 4) * 2) & 3
        if flags == 2:
            result.fail("packet end marker without packet-byte flag")
        current = bool(flags & 1)
        if previous is not None and current != previous:
            row(result, budget, "  ring segment", "logical=[%d,%d) kind=%s" % (start, logical, "packet" if previous else "stream"))
            if previous:
                result.fail("packet marker run ended without end flag")
            start = logical
        previous = current
        if flags & 2:
            row(result, budget, "  packet", "logical=[%d,%d) physicalEnd=%d" % (start, logical + 1, physical))
            start, previous = logical + 1, None
    if start < size:
        row(result, budget, "  ring segment", "logical=[%d,%d) kind=%s" % (start, size, "packet (no end observed)" if previous else "stream"))
        if previous:
            result.fail("packet tail lacks end marker")
    if graph.payload_left and size:
        data_address, _ = memory.field_address(state, "pipe_state_t", "data")
        first = min(size, capacity - head, graph.payload_left)
        graph.payload(data_address + head, first, "  logical payload")
        if graph.payload_left and first < size:
            graph.payload(data_address, min(size - first, head), "  wrapped payload")


def memory_file(graph, pointer, file):
    if int(graph.field(number(file, "inode"), "inode_t", "type")) != 0:
        raise ReadFailure("memory-file inode type mismatch")
    state = number(file, "privateData")
    inode, _ = graph.memory.field_address(state, "memory_file_t", "inode")
    if not state or number(file, "inode") != inode:
        raise ReadFailure("memory-file privateData/embedded inode mismatch")
    value = graph.read(state, "memory_file_t")
    group(graph.result, value["debugGroup"], "memory file")
    inode_value = value["inode"]
    size, object_pointer = number(inode_value, "size"), number(inode_value, "backing")
    if size > 256 * 4096 or not object_pointer:
        raise ReadFailure("invalid memory-file size/backing")
    if number(inode_value, "ioLock"):
        graph.result.fail("memory inode I/O lock held; state may be transitional")
    seals = number(value, "seals")
    row(graph.result, graph.budget, "memory-file=%#x" % state,
        "%s size=%d bytes fileOffset=%d seals=%#x (%s) backing=%#x" % (
            graph.identity(pointer), size, number(file, "offset"), seals,
            ",".join(name for bit, name in ((1, "SEAL"), (2, "SHRINK"), (4, "GROW"), (8, "WRITE"), (16, "FUTURE_WRITE")) if seals & bit) or "none", object_pointer))
    if seals & ~31:
        graph.result.fail("unknown memory-file seal bits")
    pages = {}
    for slot in range(256):
        page_pointer = int(value["pages"][slot])
        if page_pointer:
            graph.budget.take_entry()
            page = graph.read(page_pointer, "virt_shared_page_t")
            pages[slot] = page
            if number(page, "offset") != slot * 4096:
                graph.result.fail("memory-file page offset differs from physical slot")
            row(graph.result, graph.budget, "  page slot %d" % slot, "sharedPage=%#x resident=%#x offset=%d references=%d dirty=%s" % (
                page_pointer, number(page, "pointer"), number(page, "offset"), number(page, "references"), bool(page["dirty"])))
    show_backing(graph.memory, object_pointer, graph.budget, graph.result, set())
    graph.mapping_edges(object_pointer)
    if graph.payload_left and number(file, "offset") < size:
        offset = number(file, "offset")
        remaining = min(size - offset, graph.payload_left)
        while remaining:
            slot, within = divmod(offset, 4096)
            amount = min(remaining, 4096 - within)
            if slot in pages:
                graph.payload(number(pages[slot], "pointer") + within, amount, "  payload at file offset %d" % offset)
            else:
                graph.payload_left -= amount
                row(graph.result, graph.budget, "  sparse payload", "%d zero-filled hole bytes at offset %d (no target page)" % (amount, offset))
            remaining -= amount
            offset += amount


def ipc(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="descriptor-reachable IPC",
                    count_semantics="unique observed IPC objects; descriptor and queued-right roots only, not a census", units="byte offsets/sizes; readiness/seal bitmasks")
    try:
        stats = descriptor_root(memory, descriptor, "IpcDebugStats", 0, budget)
        group(result, stats["group"], "IPC manager")
        if name not in ("pipes", "memory-file"):
            raise ReadFailure("unsupported IPC view")
        graph = Graph(memory, stats, budget, result, selectors)
        graph.discover_descriptors()
        # Ancillary references can keep an object alive after every FD closes.
        sockets_pointer = number(stats, "sockets")
        if sockets_pointer and not any(key in selectors for key in ("--pid", "--tid", "--fd")):
            sockets_stats = graph.read(sockets_pointer, "SocketDebugStats")
            graph.catalogue(sockets_stats)
            for endpoint_pointer, endpoint in graph.endpoints.items():
                for packet_pointer, packet in graph.messages(endpoint_pointer, endpoint):
                    for _, _, files in graph.ancillary(packet_pointer, packet):
                        for file_pointer in files:
                            graph.selected.add(file_pointer)
                            graph.owners.setdefault(file_pointer, []).append("endpoint=%#x packet=%#x (owning queued right)" % (endpoint_pointer, packet_pointer))
        operation = number(stats, "pipeOperations" if name == "pipes" else "memoryOperations")
        expected_index = 0 if name == "pipes" else 1
        if not operation or operation != int(graph.processes["fileOperations"][expected_index]):
            raise ReadFailure("IPC operation identity differs from process provider")
        seen = set()
        for pointer in sorted(graph.selected):
            file = graph.file(pointer)
            if number(file, "operations") != operation:
                if "--fd" in selectors:
                    result.fail("selected FD has a different file-operation type; privateData remains opaque")
                continue
            private = number(file, "privateData")
            if private in seen:
                row(result, budget, "IPC alias", graph.identity(pointer) + "; object=%#x already shown" % private)
                continue
            seen.add(private)
            try:
                (pipe if name == "pipes" else memory_file)(graph, pointer, file)
            except ReadFailure as error:
                result.fail("file=%#x: %s" % (pointer, error))
        row(result, budget, "reachability", "%d unique objects observed; detached/mapping-only/in-flight references are not an exhaustive catalogue" % len(seen))
    except (ReadFailure, ValueError, OverflowError) as error:
        result.fail(error)
    return result


def sockets(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="UNIX sockets",
                    count_semantics="manager endpoint order, FIFO accepts/messages; bounded observed ownership", units="bytes; explicit UNIX address lengths; readiness masks")
    try:
        stats = descriptor_root(memory, descriptor, "SocketDebugStats", 128, budget)
        if name not in ("endpoints", "messages"):
            raise ReadFailure("unsupported sockets view")
        if name == "messages" and "--value" not in selectors:
            raise ReadFailure("messages requires one numeric endpoint from the current catalogue")
        if name == "endpoints" and "--value" in selectors:
            raise ReadFailure("numeric endpoint applies to sockets messages")
        if name == "messages" and any(key in selectors for key in ("--pid", "--tid", "--fd")):
            raise ReadFailure("messages selects an endpoint; process/FD selectors apply to endpoints")
        graph = Graph(memory, stats, budget, result, selectors)
        graph.discover_descriptors()
        graph.catalogue(stats)
        if "--value" in selectors and selectors["--value"] not in graph.endpoints:
            raise ReadFailure("selected endpoint absent from verified catalogue")
        for pointer, endpoint in graph.endpoints.items():
            if "--value" in selectors and pointer != selectors["--value"]:
                continue
            if any(key in selectors for key in ("--pid", "--tid", "--fd")) and number(endpoint, "file") not in graph.selected:
                continue
            peer = number(endpoint, "peer")
            if peer and peer not in graph.endpoints:
                result.fail("peer endpoint absent from observed catalogue")
            elif peer and number(graph.endpoints[peer], "peer") != pointer and number(endpoint, "type") != 2:
                result.fail("observed connection-oriented peer asymmetry (possibly transitional)")
            row(result, budget, "endpoint=%#x" % pointer,
                "%s %s bound=%s binding=%s connected=%s listening=%s peer=%#x local=%s peerAddress=%s backlog=%d pending=%d messages=%d queued=%d sendLimit=%d receiveLimit=%d shutdown=%s/%s internalReferences=%d passCredentials=%s credentials=%s peerCredentials=%s readiness=%#x pendingReadiness=%#x" % (
                    TYPES[number(endpoint, "type")], graph.identity(number(endpoint, "file")), bool(endpoint["bound"]), bool(endpoint["binding"]),
                    bool(endpoint["connected"]), bool(endpoint["listening"]), peer,
                    address_text(endpoint["address"], number(endpoint, "addressLength")), address_text(endpoint["peerAddress"], number(endpoint, "peerAddressLength")),
                    number(endpoint, "backlog"), number(endpoint, "pendingCount"), number(endpoint, "packetCount"), number(endpoint, "queued"),
                    number(endpoint, "sendLimit"), number(endpoint, "receiveLimit"), bool(endpoint["readShutdown"]), bool(endpoint["writeShutdown"]),
                    number(endpoint, "internalReferences"), bool(endpoint["passCredentials"]), [int(endpoint["credentials"][i]) for i in range(3)], [int(endpoint["peerCredentials"][i]) for i in range(3)],
                    number(endpoint, "lastReadiness"), number(endpoint, "pendingReadiness")))
            graph.pending(pointer, endpoint)
            for packet_pointer, packet in graph.messages(pointer, endpoint):
                row(result, budget, "  packet=%#x" % packet_pointer, "size=%d offset=%d remaining=%d source=%s (owning queue edge)" % (
                    number(packet, "size"), number(packet, "offset"), number(packet, "size") - number(packet, "offset"),
                    address_text(packet["source"], number(packet, "sourceLength"))))
                for control_pointer, control, files in graph.ancillary(packet_pointer, packet):
                    credentials = control["credentials"]
                    row(result, budget, "    ancillary=%#x" % control_pointer, "count=%d credentials pid=%d uid=%d gid=%d explicit=%s senderUid=%d" % (
                        len(files), number(credentials, "pid"), number(credentials, "uid"), number(credentials, "gid"), bool(control["explicitCredentials"]), number(control, "senderUid")))
                    for file_pointer in files:
                        row(result, budget, "      owning queued right", graph.identity(file_pointer) + "; packet=%#x; may coexist with in-flight references" % packet_pointer)
                if name == "messages" and graph.payload_left:
                    payload, _ = memory.field_address(packet_pointer, "socket_packet_t", "data")
                    graph.payload(payload + number(packet, "offset"), number(packet, "size") - number(packet, "offset"), "    remaining payload")
        row(result, budget, "ownership", "observed descriptor/accept/message owning edges and weak peers; not complete reference accounting or leak proof")
    except (ReadFailure, ValueError, OverflowError) as error:
        result.fail(error)
    return result


ipc.selectors = ("--pid", "--tid", "--fd", "--scan", "--payload-bytes")
sockets.selectors = ("--pid", "--tid", "--fd", "--scan", "--payload-bytes", "--value")
