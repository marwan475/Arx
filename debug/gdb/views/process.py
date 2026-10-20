"""Process/task identities and shared resources; bounded, stopped RAM reads only."""
from framework.results import ReadFailure, Result

from .memory import descriptor_root, number, group
from .platform import row

PROCESS_SEMANTICS = "arx.process"
TASK_SEMANTICS = "arx.task"
PROCESS_CAPACITY = 64
TASK_CAPACITY = 128
CPU_CAPACITY = 8
DESCRIPTOR_CAPACITY = 1048576
GROUP_CAPACITY = 65536
TASK_STATES = {0: "ready", 1: "running", 2: "switching", 3: "suspended", 4: "dying"}
INODE_TYPES = {0: "regular", 1: "directory", 2: "symlink", 3: "character device",
               4: "block device", 5: "socket", 6: "FIFO"}


def text_array(value, maximum):
    data = bytearray()
    for index in range(maximum):
        byte = int(value[index]) & 255
        if not byte:
            return data.decode("utf-8", "replace")
        data.append(byte)
    raise ReadFailure("unterminated bounded name/path")


def generation(value):
    debug = value["debug"]
    if number(debug, "generation") == 0 or bool(debug["exhausted"]):
        raise ReadFailure("incarnation identity unavailable or exhausted")
    return number(debug, "generation")


class Context:
    def __init__(self, memory, process_stats, task_stats, budget, result):
        self.memory = memory
        self.process_stats = process_stats
        self.task_stats = task_stats
        self.budget = budget
        self.result = result
        self.processes = {}
        self.tasks = {}
        self.process_source = number(process_stats, "source")
        if number(process_stats, "capacity") != PROCESS_CAPACITY or not self.process_source:
            raise ReadFailure("invalid process source/capacity")
        self.process_base, _ = memory.field_address(self.process_source, "ProcessManager", "Processes")
        self.task_source = number(process_stats, "tasks")
        self.task_base = 0
        if self.task_source:
            self.task_base, _ = memory.field_address(self.task_source, "TaskManager", "Tasks")
        group(result, process_stats["group"], "Process manager")
        if task_stats is not None:
            if number(task_stats, "capacity") != TASK_CAPACITY or number(task_stats, "source") != self.task_source:
                raise ReadFailure("invalid task source/capacity")
            group(result, task_stats["group"], "Task manager")
        for field, label in (("lifecycleGroup", "Lifecycle transaction"), ("credentialGroup", "Credential transaction")):
            pointer = number(process_stats, field)
            if pointer:
                group(result, memory.read(pointer, "arx_debug_group_t", budget), label)

    def address(self, kind, index):
        capacity = PROCESS_CAPACITY if kind == "process" else TASK_CAPACITY
        base = self.process_base if kind == "process" else self.task_base
        if not 0 <= index < capacity or not base:
            raise ReadFailure("%s slot outside its fixed inventory" % kind)
        return base + index * self.memory.sizeof(kind + "_t")

    def pointer_slot(self, kind, address):
        base = self.process_base if kind == "process" else self.task_base
        stride = self.memory.sizeof(kind + "_t")
        delta = address - base
        if delta < 0 or delta % stride:
            raise ReadFailure("%s pointer is not a slot address" % kind)
        index = delta // stride
        self.address(kind, index)
        return index

    def read(self, kind, index):
        cache = self.processes if kind == "process" else self.tasks
        if index not in cache:
            address = self.address(kind, index)
            allocated = int(self.memory.field(address, kind + "_t", "allocated", self.budget))
            cache[index] = self.memory.read(address, kind + "_t", self.budget) if allocated else None
        return cache[index]

    def inventory(self, kind):
        capacity = PROCESS_CAPACITY if kind == "process" else TASK_CAPACITY
        for index in range(capacity):
            value = self.read(kind, index)
            if value is not None:
                generation(value)
                if kind == "process" and number(value, "id") != index:
                    raise ReadFailure("process PID disagrees with its slot")
                yield index, value

    def process_for_task(self, task):
        pointer = number(task, "process")
        if not pointer:
            return None, None
        slot = self.pointer_slot("process", pointer)
        process = self.read("process", slot)
        if process is None or generation(process) != number(task["debug"], "processGeneration") or pointer != number(task["debug"], "processAddress"):
            raise ReadFailure("task process link is stale or partially published")
        return slot, process

    def cpu_links(self, kind, pointer):
        claims = []
        fields = ("RunningProcesses",) if kind == "process" else ("RunningTasks", "SwitchingTasks")
        source = self.process_source if kind == "process" else self.task_source
        owner_type = "ProcessManager" if kind == "process" else "TaskManager"
        for field in fields:
            array = self.memory.field(source, owner_type, field, self.budget)
            for cpu in range(CPU_CAPACITY):
                if int(array[cpu]) == pointer:
                    claims.append("%s[%d]" % (field, cpu))
        return ", ".join(claims) or "none observed"

    def parent(self, process):
        if not bool(process["hasParent"]):
            return None, "root"
        debug = process["debug"]
        identity = number(process, "parentId")
        if identity != number(debug, "parentId") or not bool(debug["parentCaptured"]):
            return None, "parent publication incomplete"
        if not 0 <= identity < PROCESS_CAPACITY:
            return None, "parent outside process inventory"
        parent = self.read("process", identity)
        if parent is None:
            return None, "parent missing (PID %d)" % identity
        if generation(parent) != number(debug, "parentGeneration"):
            return None, "stale parent incarnation (PID %d)" % identity
        return identity, "PID %d generation %d" % (identity, generation(parent))


def roots(memory, descriptor, budget, task_view, result):
    if task_view:
        task_stats = descriptor_root(memory, descriptor, "TaskDebugStats", TASK_CAPACITY, budget)
        process_source = number(task_stats, "processes")
        if not process_source:
            raise ReadFailure("process correlation unavailable")
        process_stats = memory.field(process_source, "ProcessManager", "DebugStats", budget)
    else:
        process_stats = descriptor_root(memory, descriptor, "ProcessDebugStats", PROCESS_CAPACITY, budget)
        task_source = number(process_stats, "tasks")
        task_stats = memory.field(task_source, "TaskManager", "DebugStats", budget) if task_source else None
    return Context(memory, process_stats, task_stats, budget, result)


def process_state(process):
    if bool(process["reclaiming"]):
        return "reclaiming"
    if bool(process["retirementActive"]):
        return "retiring"
    if bool(process["exited"]):
        return "exited / retained"
    return "active" if bool(process["debug"]["published"]) else "initializing / reserved"


def process_summary(context, slot, process, depth=0):
    try:
        parent_slot, parent = context.parent(process)
    except ReadFailure as error:
        context.result.fail(error)
        parent_slot, parent = None, "parent unavailable"
    if parent_slot is None and parent != "root":
        context.result.fail(parent)
    row(context.result, context.budget, "  " * depth + "PID %d" % number(process, "id"),
        "generation=%d slot=%d; %s; parent=%s; group=%d session=%d"
        % (generation(process), slot, process_state(process), parent, number(process, "groupId"), number(process, "sessionId")))


def tree(context, selected):
    records = {}
    try:
        for slot, process in context.inventory("process"):
            records[slot] = process
    except ReadFailure as error:
        context.result.fail(error)
    parents = {}
    for slot, process in records.items():
        try:
            parent, _ = context.parent(process)
            parents[slot] = parent if parent in records else None
        except ReadFailure as error:
            context.result.fail(error)
            parents[slot] = None
    visited = set()
    active = set()

    def visit(slot, depth):
        if slot in active:
            raise ReadFailure("cycle in process parent chain")
        if slot in visited:
            return
        if depth > context.budget.depth:
            raise ReadFailure("truncated: process tree depth limit")
        active.add(slot)
        visited.add(slot)
        process_summary(context, slot, records[slot], depth)
        for child in sorted(records):
            if parents[child] == slot:
                visit(child, depth + 1)
        active.remove(slot)

    if selected is not None:
        if selected not in records:
            raise ReadFailure("selected PID is not allocated")
        visit(selected, 0)
        return
    for slot in sorted(records):
        if parents[slot] is None:
            visit(slot, 0)
    for slot in sorted(records):
        if slot not in visited:
            context.result.fail("process component has no valid root (possible cycle)")
            visit(slot, 0)


def process_detail(context, slot, process):
    process_summary(context, slot, process)
    parent_generation = number(process["debug"], "parentTaskGeneration")
    parent_task = "unavailable" if not parent_generation else "slot=%d generation=%d" % (number(process["debug"], "parentTaskSlot"), parent_generation)
    wait_token = number(process, "waitOwner")
    wait_owner = "none" if not wait_token else "TID %d (claim token=%d; incarnation not inferred)" % (wait_token - 1, wait_token)
    row(context.result, context.budget, "Lifecycle",
        "parent TID=%d (captured %s); wait owner=%s; "
        "waitable=%s consumed=%s notified=%s; stopped=%s stop pending=%s continue pending=%s\n    "
        "exit status=%d; reclaiming=%s retirement active=%s; memory users=%d lifetime users=%d; "
        "address space=%#x ELF metadata=%#x"
        % (number(process, "parentTaskId"), parent_task, wait_owner, bool(process["waitable"]), bool(process["waitConsumed"]), bool(process["exitNotified"]),
           bool(process["stopped"]), bool(process["stopPending"]), bool(process["continuedPending"]), number(process, "exitStatus"),
           bool(process["reclaiming"]), bool(process["retirementActive"]), number(process, "memoryUsers"), number(process, "lifetimeUsers"),
           number(process, "addressSpace"), number(process, "elfMetadata")))
    pointer = number(process, "exitRecipient")
    recipient = "none"
    if pointer:
        index = context.pointer_slot("process", pointer)
        target = context.read("process", index)
        if target is None or generation(target) != number(process["debug"], "recipientGeneration") or pointer != number(process["debug"], "recipientAddress"):
            context.result.fail("exit recipient identity is stale or partially published")
            recipient = "unavailable"
        else:
            recipient = "PID %d generation %d (owning retained reference)" % (index, generation(target))
    row(context.result, context.budget, "Ownership",
        "process=%#x owner CPU=%d; %s; exit recipient=%s; task list=%#x (borrowed membership links)"
        % (context.address("process", slot), number(process, "ownerCpu"), context.cpu_links("process", context.address("process", slot)), recipient, number(process, "tasks")))
    # Validate bounded task membership; pointers must stay in the real task array.
    seen = set()
    previous = 0
    pointer = number(process, "tasks")
    while pointer:
        if pointer in seen:
            raise ReadFailure("cycle in process task list")
        index = context.pointer_slot("task", pointer)
        task = context.read("task", index)
        if task is None or number(task, "prev") != previous:
            raise ReadFailure("invalid task membership/previous link")
        owner, _ = context.process_for_task(task)
        if owner != slot:
            raise ReadFailure("task list and task process owner disagree")
        seen.add(pointer)
        row(context.result, context.budget, "Member task", "TID=%d slot=%d generation=%d" % (number(task, "id"), index, generation(task)))
        previous, pointer = pointer, number(task, "next")


def task_detail(context, slot, task, detail):
    process_slot, _ = context.process_for_task(task)
    state = number(task, "state")
    if state not in TASK_STATES:
        context.result.fail("unknown task state: %d" % state)
    row(context.result, context.budget, "TID %d" % number(task, "id"),
        "%s; slot=%d generation=%d; name=%s; process=%s; owner CPU=%d dispatch claims=%d; %s"
        % (TASK_STATES.get(state, "unknown"), slot, generation(task), text_array(task["name"], 16),
           "none" if process_slot is None else str(process_slot), number(task, "ownerCpu"), number(task, "dispatchUsers"),
           context.cpu_links("task", context.address("task", slot))))
    if detail:
        stack = number(task, "stack")
        if stack and stack + 65536 >= 1 << 64:
            raise ReadFailure("invalid task stack bounds")
        row(context.result, context.budget, "Stack bounds",
            "[%#x,%#x), 65536 bytes; preceding guard page is not part of the stack" % (stack, stack + 65536)
            if stack else "no stack allocated")
        row(context.result, context.budget, "Task lifetime",
            "task=%#x stack=%#x; dying=%s cleanup=%#x priority cleanup done=%s; syscall active=%d; "
            "sleep pending=%s blocked=%s; next=%#x previous=%#x"
            % (context.address("task", slot), number(task, "stack"), bool(task["dying"]), number(task, "retirementCleanup"),
               bool(task["priorityCleanupDone"]), number(task, "syscallActive"), bool(task["sleepPending"]), bool(task["sleepBlocked"]), number(task, "next"), number(task, "prev")))
        registers = task["taskContext"]
        for names in (("rip", "rsp", "rflags"), ("rbx", "rbp", "r12", "r13", "r14", "r15"), ("fsBase", "userGsBase")):
            row(context.result, context.budget, "Saved context",
                " ".join("%s=%#x" % (name, number(registers, name)) for name in names))
        address, _ = context.memory.field_address(context.address("task", slot), "task_t", "taskContext")
        row(context.result, context.budget, "Context storage",
            "arch_task_context=%#x; includes 512 saved FX-state bytes (use typed inspect for full detail)" % address)



def credentials(context, process):
    for field, label in (("user", "UID"), ("group", "GID")):
        identity = process["credentials"][field]
        row(context.result, context.budget, label,
            "real=%d effective=%d saved=%d filesystem=%d (process-wide)"
            % tuple(number(identity, name) for name in ("real", "effective", "saved", "filesystem")))
    pointer = number(process, "supplementaryGroups")
    if not pointer:
        row(context.result, context.budget, "Supplementary groups", "empty")
        return
    count = int(context.memory.field(pointer, "process_group_list_t", "count", context.budget))
    references = int(context.memory.field(pointer, "process_group_list_t", "references", context.budget))
    if not 0 <= count <= GROUP_CAPACITY or references <= 0:
        raise ReadFailure("invalid supplementary group bounds/refcount")
    base, _ = context.memory.field_address(pointer, "process_group_list_t", "ids")
    row(context.result, context.budget, "Supplementary groups", "list=%#x count=%d references=%d; immutable shared list" % (pointer, count, references))
    for index in range(count):
        context.budget.take_entry()
        identity = int(context.memory.read(base + index * 4, "uint32_t", context.budget))
        context.result.items.append({"path": "Group", "type": "GID", "text": str(identity)})


def observers(context, file):
    row(context.result, context.budget, "Weak close observer",
        "callback=%#x context=%#x" % (number(file, "closeObserver"), number(file, "closeObserverContext")))
    pointer = number(file, "readinessObservers")
    seen = set()
    while pointer:
        if pointer in seen:
            raise ReadFailure("cycle in weak readiness observers")
        seen.add(pointer)
        context.budget.take_entry()
        observer = context.memory.read(pointer, "file_close_observer_t", context.budget)
        context.result.items.append({"path": "Weak readiness observer", "type": "non-owning registration",
                                     "text": "record=%#x callback=%#x context=%#x" % (pointer, number(observer, "Closing"), number(observer, "context"))})
        pointer = number(observer, "next")


def file_detail(context, pointer):
    file = context.memory.read(pointer, "file_t", context.budget)
    operations = number(file, "operations")
    kind = "unknown implementation"
    for index, label in enumerate(("pipe", "memory file", "UNIX socket")):
        if operations and operations == int(context.process_stats["fileOperations"][index]):
            kind = label
    if operations:
        poll = int(context.memory.field(operations, "file_operations_t", "Poll", context.budget))
        for index, label in enumerate(("event counter", "epoll")):
            if poll and poll == int(context.process_stats["filePolls"][index]):
                kind = label
    row(context.result, context.budget, "Open file",
        "file=%#x kind=%s; offset=%d bytes status=%#x references=%d operations=%#x; "
        "inode=%#x mount=%#x dentry=%#x private data=%#x (opaque)"
        % (pointer, kind, number(file, "offset"), number(file, "statusFlags"), number(file, "refCount"), operations,
           number(file, "inode"), number(file["path"], "mount"), number(file["path"], "dentry"), number(file, "privateData")))
    inode_pointer = number(file, "inode")
    if inode_pointer:
        inode = context.memory.read(inode_pointer, "inode_t", context.budget)
        row(context.result, context.budget, "Inode",
            "address=%#x type=%s; number=%d size=%d bytes; mode=%#o UID=%d GID=%d; backing=%#x"
            % (inode_pointer, INODE_TYPES.get(number(inode, "type"), "unknown"), number(inode, "inodeNumber"),
               number(inode, "size"), number(inode, "mode"), number(inode, "uid"), number(inode, "gid"), number(inode, "backing")))
    observers(context, file)


def files(context, process, task, scan):
    defaults = {"descriptorTable": number(process, "descriptorTable"), "filesystemContext": number(process, "filesystemContext")}
    selected = {field: number(task, field) if task is not None and number(task, field) else pointer for field, pointer in defaults.items()}
    row(context.result, context.budget, "Selected contexts",
        "source=%s; table=%#x (process default=%#x); filesystem=%#x (process default=%#x)"
        % ("task overrides with process fallback" if task is not None else "process defaults",
           selected["descriptorTable"], defaults["descriptorTable"], selected["filesystemContext"], defaults["filesystemContext"]))
    # Report observed owning edges once each; table sharing does not multiply
    # direct descriptor->file references, and does not prove exhaustive ownership.
    table_owners = {}
    for kind in ("process", "task"):
        for slot, value in context.inventory(kind):
            owner = "%s slot=%d generation=%d" % (kind, slot, generation(value))
            table_pointer = number(value, "descriptorTable")
            if table_pointer:
                table_owners.setdefault(table_pointer, []).append(owner)
            for field, pointer in selected.items():
                if pointer and number(value, field) == pointer:
                    row(context.result, context.budget, "Observed context owner",
                        "%s slot=%d generation=%d -> %s=%#x (owning reference)" % (kind, slot, generation(value), field, pointer))
    filesystem = selected["filesystemContext"]
    if filesystem:
        fs = context.memory.read(filesystem, "filesystem_context_t", context.budget)
        row(context.result, context.budget, "Filesystem context",
            "address=%#x references=%d umask=%#o cwd=%s"
            % (filesystem, number(fs, "references"), number(fs, "umask"), text_array(fs["cwdPath"], 256)))
    pointer = selected["descriptorTable"]
    if not pointer:
        row(context.result, context.budget, "Descriptors", "no selected table")
        return
    table = context.memory.read(pointer, "descriptor_table_t", context.budget)
    count = number(table, "count")
    base = number(table, "entries")
    if not 0 <= count <= DESCRIPTOR_CAPACITY or (count and not base) or number(table, "references") <= 0:
        raise ReadFailure("invalid descriptor allocation capacity/root/refcount")
    row(context.result, context.budget, "Descriptor table",
        "table=%#x references=%d capacity=%d slots; scan ceiling=%d slots; process soft limit=%d; "
        "observed owners exclude in-flight references"
        % (pointer, number(table, "references"), count, scan, number(process["fileLimit"], "current")))
    seen = set()
    scanned = opened = 0
    for index in range(min(count, scan)):
        descriptor = context.memory.read(base + index * context.memory.sizeof("file_descriptor_t"), "file_descriptor_t", context.budget)
        scanned += 1
        file = number(descriptor, "file")
        if not file:
            continue
        opened += 1
        route = number(descriptor, "operations")
        row(context.result, context.budget, "FD %d" % index,
            "file=%#x flags=%#x CLOEXEC=%s route=%#x (owning descriptor reference)"
            % (file, number(descriptor, "flags"), bool(number(descriptor, "flags") & 1), route))
        # Deduplicate by route too: an opaque custom handle cannot authorize
        # decoding a later descriptor with a different route.
        identity = route, file
        if identity in seen:
            continue
        seen.add(identity)
        if route and route == number(context.process_stats, "vfsRoute"):
            file_detail(context, file)
        else:
            row(context.result, context.budget, "Opaque file handle", "unverified/custom route; no file_t interpretation")
    row(context.result, context.budget, "Descriptor scan", "%d/%d slots scanned; %d occupied entries observed" % (scanned, count, opened))
    if scanned != count:
        context.result.fail("truncated: descriptor slot scan ceiling; open count is not exhaustive")
    # A copied table may own the same open file even when it is not the selected
    # task's shared table. Correlate those direct edges once per table, within
    # the same global slot-scan and byte ceilings; never equate this with refCount.
    remaining = scan - scanned
    correlated = 0
    for other, owners in sorted(table_owners.items()):
        if other == pointer or not seen:
            continue
        if remaining == 0:
            context.result.fail("truncated: file-reference scan ceiling; other table owners may exist")
            break
        candidate = context.memory.read(other, "descriptor_table_t", context.budget)
        capacity = number(candidate, "count")
        entries = number(candidate, "entries")
        if not 0 <= capacity <= DESCRIPTOR_CAPACITY or (capacity and not entries) or number(candidate, "references") <= 0:
            raise ReadFailure("invalid correlated descriptor table bounds")
        limit = min(capacity, remaining)
        for index in range(limit):
            descriptor = context.memory.read(entries + index * context.memory.sizeof("file_descriptor_t"), "file_descriptor_t", context.budget)
            identity = number(descriptor, "operations"), number(descriptor, "file")
            if identity in seen:
                row(context.result, context.budget, "Observed file owner",
                    "table=%#x FD=%d -> file=%#x route=%#x; owning descriptor edge; table owners=%s"
                    % (other, index, identity[1], identity[0], "; ".join(owners)))
        remaining -= limit
        correlated += limit
        if limit != capacity:
            context.result.fail("truncated: file-reference scan ceiling; owner correlation is incomplete")
            break
    row(context.result, context.budget, "Reference scan",
        "%d additional descriptor slots checked; selected and other tables share the %d-slot ceiling"
        % (correlated, scan))


def gather(name, memory, descriptor, budget, selectors=None, task_view=False):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="Tasks" if task_view else "Processes",
                    count_semantics="current incarnations; no reclaimed history or complete reference accounting",
                    units="IDs, generations, slots and reference counts are distinct")
    try:
        context = roots(memory, descriptor, budget, task_view, result)
        identity = selectors.get("--tid" if task_view else "--pid")
        if task_view and "--pid" in selectors or not task_view and "--tid" in selectors:
            raise ReadFailure("selector must match process PID or task TID")
        if "--scan" in selectors and name != "files":
            raise ReadFailure("--scan is only supported by file views")
        if not task_view and name == "tree":
            tree(context, identity)
            return result
        if name != "list" and identity is None:
            raise ReadFailure("selected details/files/credentials requires --pid or --tid")
        kind = "task" if task_view else "process"
        candidates = context.inventory(kind)
        if identity is not None:
            indices = []
            capacity = TASK_CAPACITY if task_view else PROCESS_CAPACITY
            for index in range(capacity):
                address = context.address(kind, index)
                if int(memory.field(address, kind + "_t", "allocated", budget)):
                    current_id = int(memory.field(address, kind + "_t", "id", budget))
                    if current_id == identity:
                        indices.append(index)
            if len(indices) > 1:
                raise ReadFailure("selected identity is ambiguous across allocated slots")
            candidates = ((index, context.read(kind, index)) for index in indices)
        matched = False
        for slot, value in candidates:
            if identity is not None and number(value, "id") != identity:
                continue
            matched = True
            if task_view:
                process_slot, process = context.process_for_task(value)
                if name == "files":
                    if process is None:
                        raise ReadFailure("selected task has no process context")
                    files(context, process, value, selectors.get("--scan", 256))
                else:
                    task_detail(context, slot, value, name == "details")
            elif name == "files":
                files(context, value, None, selectors.get("--scan", 256))
            elif name == "credentials":
                credentials(context, value)
            else:
                process_detail(context, slot, value)
            if identity is not None:
                break
        if identity is not None and not matched:
            result.fail("selected identity is not currently allocated")
    except ReadFailure as error:
        result.fail(error)
    return result


def process(name, memory, descriptor, budget, selectors=None):
    return gather(name, memory, descriptor, budget, selectors)


def task(name, memory, descriptor, budget, selectors=None):
    return gather(name, memory, descriptor, budget, selectors, task_view=True)


process.selectors = ("--pid", "--scan")
task.selectors = ("--tid", "--scan")
