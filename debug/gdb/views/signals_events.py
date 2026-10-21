"""Stopped-memory signal/event readers; native contracts live in stats headers."""
from framework.results import ReadFailure, Result
from .memory import descriptor_root, group, number
from .platform import row
from .process import Context, generation
from .communication import Graph

SIGNALS = "arx.signals"
EVENTS = "arx.events"
UNBLOCKABLE = (1 << 8) | (1 << 18)
STANDARD = (1 << 31) - 1
SIGNAL_NAMES = (
    "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL",
    "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD",
    "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ",
    "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
)
POLL_BITS = {
    0: "IN", 1: "PRI", 2: "OUT", 3: "ERR", 4: "HUP", 5: "NVAL",
    6: "RDNORM", 7: "RDBAND", 8: "WRNORM", 9: "WRBAND", 13: "RDHUP",
    28: "EXCLUSIVE", 29: "WAKEUP", 30: "ONESHOT", 31: "ET",
}
INOTIFY_BITS = {
    0: "ACCESS", 1: "MODIFY", 2: "ATTRIB", 3: "CLOSE_WRITE",
    4: "CLOSE_NOWRITE", 5: "OPEN", 6: "MOVED_FROM", 7: "MOVED_TO",
    8: "CREATE", 9: "DELETE", 10: "DELETE_SELF", 11: "MOVE_SELF",
    13: "UNMOUNT", 14: "OVERFLOW", 15: "IGNORED", 24: "ONLYDIR",
    25: "DONT_FOLLOW", 26: "EXCL_UNLINK", 28: "MASK_CREATE",
    29: "MASK_ADD", 30: "ISDIR", 31: "ONESHOT",
}
ACTION_BITS = {
    0: "NOCLDSTOP", 1: "NOCLDWAIT", 2: "SIGINFO", 26: "RESTORER",
    27: "ONSTACK", 28: "RESTART", 30: "NODEFER", 31: "RESETHAND",
}


def signal_name(signal):
    return SIGNAL_NAMES[signal - 1] if signal < 32 else "RTMIN+%d" % (signal - 32)


def signal_mask(mask):
    return "%#x [%s]" % (mask, "|".join(signal_name(i) for i in range(1, 65)
                                      if mask & (1 << (i - 1))) or "none")


def flags(mask, meanings):
    labels = [name for bit, name in meanings.items() if mask & (1 << bit)]
    unknown = mask & ~sum(1 << bit for bit in meanings)
    if unknown:
        labels.append("unknown=%#x" % unknown)
    return "%#x [%s]" % (mask, "|".join(labels) or "none")


def sender_text(sender):
    return "sender pid=%d uid=%d code=%d value=%#x" % tuple(
        number(sender, field) for field in ("pid", "uid", "code", "value"))


def signal_context(memory, stats, budget, result):
    pointer = number(stats, "processes")
    if not pointer:
        raise ReadFailure("process identities unavailable")
    processes = memory.read(pointer, "ProcessDebugStats", budget)
    tasks = number(processes, "taskStats")
    task_stats = memory.read(tasks, "TaskDebugStats", budget) if tasks else None
    return Context(memory, processes, task_stats, budget, result)


def selected_contexts(context, selectors):
    """Yield process slot/value plus selected associated tasks, with incarnations."""
    if "--pid" in selectors and "--tid" in selectors:
        raise ReadFailure("--pid and --tid are mutually exclusive")
    tasks = {}
    matched = False
    for slot, task in context.inventory("task"):
        if "--tid" in selectors and number(task, "id") != selectors["--tid"]:
            continue
        pid, process = context.process_for_task(task)
        if process is None:
            if "--tid" in selectors:
                raise ReadFailure("selected task has no published process")
            continue
        tasks.setdefault(pid, []).append((slot, task))
        matched = True
    if "--tid" in selectors and not matched:
        raise ReadFailure("selected task is not allocated")
    found = False
    for pid, process in context.inventory("process"):
        if selectors.get("--pid", pid) != pid:
            continue
        if "--tid" in selectors and pid not in tasks:
            continue
        found = True
        yield pid, process, tasks.get(pid, [])
    if not found and any(key in selectors for key in ("--pid", "--tid")):
        raise ReadFailure("selected process is not allocated")


def signal_storage(memory, pointer, kind, value, budget, result, label):
    pending = number(value, "signalPending")
    row(result, budget, label, "pending=" + signal_mask(pending))
    queue = number(value, "signalQueue")
    observed = 0
    if not queue:
        row(result, budget, label + " queue", "null/unpublished; count=0 reserved=0 observed")
    else:
        count = int(memory.field(queue, "signal_queue_t", "count", budget))
        reserved = int(memory.field(queue, "signal_queue_t", "reserved", budget))
        if not 0 <= count <= 128 or not 0 <= reserved <= 128 - count:
            raise ReadFailure("invalid signal queue count/reserved capacity")
        row(result, budget, label + " queue=%#x" % queue,
            "count=%d reserved=%d capacity=128 compact enqueue order" % (count, reserved))
        base, _ = memory.field_address(queue, "signal_queue_t", "records")
        for index in range(count):
            budget.take_entry()
            record = memory.read(base + index * memory.sizeof("signal_queue_t::record_t"),
                                 "signal_queue_t::record_t", budget)
            signal = number(record, "number")
            if not 32 <= signal <= 64:
                raise ReadFailure("invalid real-time signal queue record")
            observed |= 1 << (signal - 1)
            row(result, budget, label + " queue[%d]" % index,
                signal_name(signal) + " " + sender_text(record["sender"]))
    if pending & ~STANDARD != observed:
        result.fail(label + " RT pending bits differ from observed queue (partial update)")
    if kind == "process":
        base, _ = memory.field_address(pointer, "process_t", "signalSenders")
    else:
        base = number(value, "signalSenders")
    for signal in range(1, 32):
        if pending & (1 << (signal - 1)):
            if not base:
                raise ReadFailure("pending standard signal has null/unpublished sender storage")
            budget.take_entry()
            sender = memory.read(base + (signal - 1) * memory.sizeof("signal_sender_t"),
                                 "signal_sender_t", budget)
            row(result, budget, label + " " + signal_name(signal), sender_text(sender))


def signals(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="signals",
                    count_semantics="allocated process/task state; pending bits summarize queues")
    try:
        if name not in ("pending", "dispositions"):
            raise ReadFailure("unsupported signals view")
        stats = descriptor_root(memory, descriptor, "SignalDebugStats", 0, budget)
        if not number(stats, "source"):
            raise ReadFailure("signal manager unpublished")
        group(result, stats["group"], "signal manager")
        context = signal_context(memory, stats, budget, result)
        seen = set()
        for pid, process, tasks in selected_contexts(context, selectors):
            pointer = context.address("process", pid)
            row(result, budget, "PID %d" % pid,
                "process=%#x generation=%d stopped=%s stopSignal=%d stopSequence=%d exited=%s terminationSignal=%d exitRecipient=%#x" % (
                    pointer, generation(process), bool(process["stopped"]), number(process, "stopSignal"),
                    number(process, "stopSequence"), bool(process["exited"]),
                    number(process, "terminationSignal"), number(process, "exitRecipient")))
            if number(process, "resourceLock"):
                result.fail("process resource lock held; dispositions may be detaching")
            dispositions_pointer = number(process, "signalDispositions")
            if not dispositions_pointer:
                result.fail("process dispositions null/unpublished/retired; no default table inferred")
                dispositions = None
            else:
                dispositions = memory.read(dispositions_pointer, "signal_dispositions_t", budget)
                if not number(dispositions, "references"):
                    raise ReadFailure("signal dispositions have no retained references")
                if number(dispositions, "lock"):
                    result.fail("signal disposition lock held; signal state may be transitional")
                row(result, budget, "  dispositions=%#x" % dispositions_pointer,
                    "references=%d ignored=%s" % (number(dispositions, "references"),
                                                  signal_mask(number(dispositions, "ignored"))))
            if name == "dispositions":
                if dispositions is None or dispositions_pointer in seen:
                    continue
                seen.add(dispositions_pointer)
                for index in range(64):
                    action = dispositions["actions"][index]
                    handler = number(action, "handler")
                    row(result, budget, "  " + signal_name(index + 1),
                        "handler=%s flags=%s restorer=%#x mask=%s" % (
                            "default" if handler == 0 else "ignore" if handler == 1 else "%#x" % handler,
                            flags(number(action, "flags"), ACTION_BITS), number(action, "restorer"),
                            signal_mask(number(action, "mask"))))
                continue
            signal_storage(memory, pointer, "process", process, budget, result, "  process-directed")
            for slot, task in tasks:
                task_pointer = context.address("task", slot)
                signal_storage(memory, task_pointer, "task", task, budget, result,
                               "    TID %d task-directed" % number(task, "id"))
                stack = task["signalStack"]
                combined = number(process, "signalPending") | number(task, "signalPending")
                eligible = None if dispositions is None or result.issues else (
                    combined & ~(number(task, "signalMask") & ~UNBLOCKABLE) & ~number(dispositions, "ignored"))
                sequence, _ = memory.field_address(task_pointer, "task_t", "signalSequence")
                row(result, budget, "  TID %d" % number(task, "id"),
                    "task=%#x slot=%d generation=%d masked=%s saved=%s restore=%s depth=%d sequence=%d waitSequence=%#x state=%d eligible=%s altStack=%#x size=%d flags=%#x" % (
                        task_pointer, slot, generation(task), signal_mask(number(task, "signalMask")),
                        signal_mask(number(task, "signalSavedMask")), bool(task["signalRestoreMask"]),
                        number(task, "signalDepth"), number(task, "signalSequence"), sequence,
                        number(task, "state"), "unknown (partial state)" if eligible is None else signal_mask(eligible),
                        number(stack, "sp"), number(stack, "size"), number(stack, "flags")))
    except (ReadFailure, ValueError, OverflowError) as error:
        result.fail(error)
    return result


class EventReader:
    def __init__(self, memory, stats, budget, result, selectors):
        self.memory, self.stats, self.budget, self.result = memory, stats, budget, result
        self.selectors = selectors
        self.source = number(stats, "source")
        if not self.source:
            raise ReadFailure("event manager unpublished")
        self.scan_left = selectors.get("--scan", 4096)
        if not 1 <= self.scan_left <= 1048576:
            raise ReadFailure("invalid event scan ceiling")
        self.graph = None
        self.epolls = {}
        self.watches = {}
        self.shown_files = set()

    def read(self, pointer, type_name):
        if pointer <= 0 or pointer % 8 or pointer + self.memory.sizeof(type_name) > 1 << 64:
            raise ReadFailure("null/misaligned/overflowing " + type_name + " pointer")
        return self.memory.read(pointer, type_name, self.budget)

    def field(self, pointer, type_name, name):
        if not pointer:
            raise ReadFailure("null " + type_name + " pointer")
        return self.memory.field(pointer, type_name, name, self.budget)

    def scan(self):
        if self.scan_left <= 0:
            raise ReadFailure("truncated: event scan ceiling")
        self.scan_left -= 1

    def chain(self, pointer, type_name, maximum=None):
        seen = set()
        while pointer:
            self.scan()
            if pointer in seen:
                raise ReadFailure("cycle in " + type_name)
            if maximum is not None and len(seen) >= maximum:
                raise ReadFailure("invalid/excessive " + type_name + " count")
            seen.add(pointer)
            self.budget.take_entry()
            value = self.read(pointer, type_name)
            yield pointer, value
            pointer = number(value, "next")

    def descriptors(self):
        self.graph = Graph(self.memory, self.stats, self.budget, self.result, self.selectors)
        self.graph.discover_descriptors()
        self.scan_left = self.graph.scan_left

    def selected(self, file):
        return self.graph is None or file in self.graph.selected

    def file(self, pointer):
        file = self.read(pointer, "file_t")
        if number(file, "refCount") == 0:
            raise ReadFailure("expired/retiring weak file; no retained references")
        if number(file, "ioLock") or number(file, "refLock"):
            self.result.fail("file lock held; lifetime/state may be transitional")
        return file

    def identity(self, pointer, type_name, file_pointer, operation):
        file = self.file(file_pointer)
        inode, _ = self.memory.field_address(pointer, type_name, "inode")
        expected = number(self.stats, operation)
        if (not expected or number(file, "operations") != expected
                or number(file, "privateData") != pointer or number(file, "inode") != inode):
            raise ReadFailure("file operations/privateData/inode identity mismatch")
        if int(self.field(inode, "inode_t", "type")) != 0:
            raise ReadFailure("anonymous event inode type mismatch")
        return file

    def observer(self, pointer, type_name, file_pointer, callback):
        location, _ = self.memory.field_address(pointer, type_name, "observer")
        observed = self.read(location, "file_close_observer_t")
        if (number(observed, "context") != pointer
                or not number(self.stats, callback)
                or number(observed, "Closing") != number(self.stats, callback)):
            raise ReadFailure("weak close observer identity mismatch")
        file = self.file(file_pointer)
        for address, _ in self.chain(number(file, "readinessObservers"), "file_close_observer_t"):
            if address == location:
                return location
        raise ReadFailure("stale weak file: close observer absent from file")

    def epoll_catalogue(self):
        root = int(self.field(self.source, "EventManager", "Epolls"))
        for pointer, epoll in self.chain(root, "event_epoll_t"):
            if number(epoll, "manager") != self.source:
                raise ReadFailure("epoll manager identity mismatch")
            if not 0 <= number(epoll, "topologyDepth") <= 5:
                raise ReadFailure("invalid stored epoll topology depth")
            self.identity(pointer, "event_epoll_t", number(epoll, "file"), "epollOperations")
            self.observer(pointer, "event_epoll_t", number(epoll, "file"), "epollClosing")
            self.epolls[pointer] = epoll

    def epoll(self, pointer, path=()):
        if pointer in path:
            raise ReadFailure("cycle in nested epoll topology")
        if len(path) >= min(5, self.budget.depth):
            raise ReadFailure("truncated: nested epoll depth")
        if pointer not in self.epolls:
            raise ReadFailure("nested epoll absent from verified manager catalogue")
        epoll = self.epolls[pointer]
        self.shown_files.add(number(epoll, "file"))
        sequence_address, _ = self.memory.field_address(pointer, "event_epoll_t", "sequence")
        row(self.result, self.budget, "epoll=%#x" % pointer,
            "file=%#x sequence=%d waitSequence=%#x topologyGeneration=%d topologyDepth=%d notificationGeneration=%d; stored hints only" % (
                number(epoll, "file"), number(epoll, "sequence"), sequence_address, number(epoll, "topologyGeneration"),
                number(epoll, "topologyDepth"), number(epoll, "notificationGeneration")))
        if self.graph is not None:
            row(self.result, self.budget, "  epoll owners",
                "; ".join(self.graph.owners.get(number(epoll, "file"), [])) or "no descriptor owner observed")
        if pointer not in self.watches:
            self.watches[pointer] = list(self.chain(number(epoll, "head"), "event_epoll_watch_t"))
        watches = self.watches[pointer]
        if number(epoll, "tail") != (watches[-1][0] if watches else 0):
            self.result.fail("epoll tail differs from observed watch order")
        for watch_pointer, watch in watches:
            if number(watch, "owner") != pointer:
                raise ReadFailure("epoll watch owner mismatch")
            file_pointer = number(watch, "file")
            observer = self.observer(watch_pointer, "event_epoll_watch_t", file_pointer, "watchClosing")
            file = self.file(file_pointer)
            row(self.result, self.budget, "  watch=%#x" % watch_pointer,
                "storedFd=%d weakFile=%#x observer=%#x events=%s data=%#x armed=%s pending=%s" % (
                    number(watch, "descriptor"), file_pointer, observer,
                    flags(number(watch["event"], "events"), POLL_BITS), number(watch["event"], "data"),
                    bool(watch["armed"]), bool(watch["pending"])))
            if number(file, "operations") == number(self.stats, "epollOperations"):
                child = number(file, "privateData")
                self.identity(child, "event_epoll_t", file_pointer, "epollOperations")
                self.epoll(child, path + (pointer,))
            elif self.graph is not None:
                row(self.result, self.budget, "    source owners",
                    "; ".join(self.graph.owners.get(file_pointer, [])) or "no descriptor owner observed")

    def inotify(self, pointer):
        count = int(self.field(pointer, "inotify_state_t", "count"))
        head = int(self.field(pointer, "inotify_state_t", "head"))
        if not 0 <= count <= 64 or not 0 <= head < 64:
            raise ReadFailure("invalid inotify ring head/count")
        row(self.result, self.budget, "  inotify", "head=%d queued=%d capacity=64 FIFO order" % (head, count))
        base, _ = self.memory.field_address(pointer, "inotify_state_t", "watches")
        for index in range(128):
            self.scan()
            address = base + index * self.memory.sizeof("notification_watch_t")
            watch = self.read(address, "notification_watch_t")
            if number(watch, "inode"):
                row(self.result, self.budget, "    watch slot=%d" % index,
                    "wd=%d inode=%#x mask=%s" % (number(watch, "descriptor"), number(watch, "inode"),
                                                flags(number(watch, "mask"), INOTIFY_BITS)))
        base, _ = self.memory.field_address(pointer, "inotify_state_t", "records")
        overflow = False
        for index in range(count):
            self.scan()
            physical = (head + index) % 64
            record = self.read(base + physical * self.memory.sizeof("notification_record_t"), "notification_record_t")
            length = number(record, "length")
            if not 0 <= length <= 256 or length % 4:
                raise ReadFailure("invalid inotify record name length")
            data = bytes(int(record["name"][i]) & 255 for i in range(length))
            if length and b"\0" not in data:
                raise ReadFailure("unterminated bounded inotify name")
            overflow |= bool(number(record, "mask") & 0x4000)
            row(self.result, self.budget, "    record[%d] physical=%d" % (index, physical),
                "wd=%d mask=%s cookie=%d length=%d name=%r" % (
                    number(record, "descriptor"), flags(number(record, "mask"), INOTIFY_BITS),
                    number(record, "cookie"), length, data.split(b"\0", 1)[0]))
        row(self.result, self.budget, "  overflow", "%s (observed queued marker; not persistent history)" % overflow)

    def signalfd(self, pointer):
        mask = int(self.field(pointer, "signal_descriptor_t", "mask"))
        row(self.result, self.budget, "  signalfd", "mask=" + signal_mask(mask))
        if self.graph is None:
            row(self.result, self.budget, "  matching", "select --tid for caller-context matching; shared descriptor has no intrinsic recipient")
            return
        signal_stats = number(self.stats, "signals")
        if signal_stats:
            group(self.result, self.read(signal_stats, "SignalDebugStats")["group"], "signal manager")
        for pid, process, tasks in selected_contexts(self.graph.context, self.selectors):
            dispositions = number(process, "signalDispositions")
            if (number(process, "resourceLock") or not dispositions
                    or int(self.field(dispositions, "signal_dispositions_t", "lock"))):
                self.result.fail("signal resource/disposition lock held or unpublished; matching is transitional")
            if not tasks:
                row(self.result, self.budget, "    PID %d process-only match" % pid,
                    signal_mask(number(process, "signalPending") & mask))
            for _, task in tasks:
                matching = (number(process, "signalPending") | number(task, "signalPending")) & mask
                row(self.result, self.budget, "    PID %d TID %d context" % (pid, number(task, "id")),
                    "processMatch=%s taskMatch=%s combined=%s; caller mask not applied; no consumption" % (
                        signal_mask(number(process, "signalPending") & mask),
                        signal_mask(number(task, "signalPending") & mask), signal_mask(matching)))

    def notifications(self):
        root = int(self.field(self.source, "EventManager", "Notifications"))
        for pointer, common in self.chain(root, "event_notification_t", 64):
            if number(common, "manager") != self.source:
                raise ReadFailure("notification manager identity mismatch")
            kind = number(common, "kind")
            if kind not in (1, 2):
                raise ReadFailure("invalid notification kind")
            file_pointer = number(common, "file")
            self.identity(pointer, "event_notification_t", file_pointer,
                          "inotifyOperations" if kind == 1 else "signalOperations")
            if not self.selected(file_pointer):
                continue
            self.shown_files.add(file_pointer)
            row(self.result, self.budget, "notification=%#x" % pointer,
                "kind=%s file=%#x global manager list" % ("inotify" if kind == 1 else "signalfd", file_pointer))
            (self.inotify if kind == 1 else self.signalfd)(pointer)

    def counters(self):
        for file_pointer in sorted(self.graph.selected):
            file = self.file(file_pointer)
            expected = number(self.stats, "counterOperations")
            if not expected or number(file, "operations") != expected:
                if "--fd" in self.selectors:
                    self.result.fail("selected FD is not an event counter; privateData remains opaque")
                continue
            pointer = number(file, "privateData")
            self.identity(pointer, "event_counter_t", file_pointer, "counterOperations")
            counter = self.read(pointer, "event_counter_t")
            if number(counter, "manager") != self.source or not 0 <= number(counter, "value") <= (1 << 64) - 2:
                raise ReadFailure("invalid event counter manager/value")
            if number(counter, "lock"):
                self.result.fail("counter lock held; counter update interrupted")
            self.shown_files.add(file_pointer)
            row(self.result, self.budget, "counter=%#x" % pointer,
                "file=%#x value=%d semaphore=%s; descriptor-discovered; %s" % (
                    file_pointer, number(counter, "value"), bool(counter["semaphore"]),
                    "; ".join(self.graph.owners.get(file_pointer, []))))


def events(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="events",
                    count_semantics="stored manager lists and descriptor-discovered counters; no readiness calls")
    try:
        if name not in ("epoll", "notifications", "counter"):
            raise ReadFailure("unsupported events view")
        stats = descriptor_root(memory, descriptor, "EventDebugStats", 0, budget)
        group(result, stats["group"], "event manager")
        reader = EventReader(memory, stats, budget, result, selectors)
        selecting = any(key in selectors for key in ("--pid", "--tid", "--fd"))
        if selecting or name == "counter":
            reader.descriptors()
        if name == "counter":
            reader.counters()
        elif name == "notifications":
            if int(reader.field(reader.source, "EventManager", "NotificationLock")):
                result.fail("notification lock held; list/ring update interrupted")
            reader.notifications()
        else:
            lock = number(stats, "readinessLock")
            if not lock or int(memory.read(lock, "spinlock_t", budget)):
                result.fail("readiness lock unavailable/held; weak topology may be transitional")
            for field in ("Generation", "TopologyGeneration", "NotificationGeneration", "ExclusiveGeneration"):
                row(result, budget, field, str(int(reader.field(reader.source, "EventManager", field))))
            generation_address, _ = memory.field_address(reader.source, "EventManager", "Generation")
            row(result, budget, "general waitSequence", "%#x" % generation_address)
            reader.epoll_catalogue()
            for pointer, epoll in reader.epolls.items():
                if reader.selected(number(epoll, "file")):
                    reader.epoll(pointer)
        if "--fd" in selectors and not reader.shown_files:
            result.fail("selected FD has no matching verified event type; privateData remains opaque")
        if selecting and name in ("epoll", "notifications"):
            row(result, budget, "ownership", "selected descriptor contexts; nested weak sources may have other owners")
    except (ReadFailure, ValueError, OverflowError) as error:
        result.fail(error)
    return result


signals.selectors = ("--pid", "--tid")
events.selectors = ("--pid", "--tid", "--fd", "--scan")
