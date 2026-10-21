"""Stopped-RAM scheduling/wait/clock observations; contracts in stats headers."""
from framework import readers
from framework.results import ReadFailure, Result
from .memory import descriptor_root, group, number
from .platform import row
from .process import Context, generation, TASK_STATES

SCHEDULER = "arx.scheduler"
SYNC = "arx.sync"
TIME = "arx.time"
NONE = (1 << 64) - 1
POLICIES = {0: "OTHER", 1: "FIFO", 2: "RR", 6: "DEADLINE"}
SLEEP_TYPE = "Scheduler::sleep_entry_t"
WAITER_TYPE = "SynchronizationManager::waiter_t"
STATE_TYPE = "SynchronizationManager::pi_state_t"
PI_TYPE = "SynchronizationManager::pi_waiter_t"


def policy(value):
    return POLICIES.get(value, "invalid(%d)" % value)


class Observation:
    def __init__(self, memory, stats, budget, result):
        self.memory, self.stats, self.budget, self.result = memory, stats, budget, result
        self.groups = []
        self.watch(number(stats, "source"), stats["group"], "manager")
        process_address = number(stats, "processStats")
        if not process_address:
            raise ReadFailure("process identities unavailable")
        processes = self.read(process_address, "ProcessDebugStats")
        task_address = number(processes, "taskStats")
        tasks = self.read(task_address, "TaskDebugStats") if task_address else None
        if tasks is None:
            raise ReadFailure("task identities unavailable")
        task_pointer = number(stats, "taskStats")
        if task_pointer != task_address:
            raise ReadFailure("manager task identity root disagrees with process root")
        self.context = Context(memory, processes, tasks, budget, result)
        self.policies = {}

    def read(self, address, type_name):
        if address <= 0 or address + self.memory.sizeof(type_name) > (1 << 64):
            raise ReadFailure("null/overflowing %s pointer" % type_name)
        return self.memory.read(address, type_name, self.budget)

    def scalar(self, field, type_name="uint64_t", index=0):
        base = number(self.stats, field)
        if not base:
            raise ReadFailure("unavailable: null scalar/CPU root")
        return int(self.read(base + index * self.memory.sizeof(type_name), type_name))

    def watch(self, address, value, label):
        group(self.result, value, label)
        # Top-level manager is re-read at finish; no spinning on active writers.
        self.groups.append((address, number(value, "completedUpdates"), label))

    def finish(self, root_type):
        address, completed, label = self.groups[0]
        latest = self.memory.field(address, root_type, "DebugStats", self.budget)["group"]
        group(self.result, latest, label)
        if number(latest, "completedUpdates") != completed:
            self.result.fail("manager changed during observation")

    def task(self, address):
        slot = self.context.pointer_slot("task", address)
        value = self.context.read("task", slot)
        if value is None:
            raise ReadFailure("task pointer names an unallocated slot")
        incarnation = generation(value)
        pid, process = self.context.process_for_task(value)
        return slot, value, pid, process, "slot=%d tid=%d generation=%d pid=%s" % (
            slot, number(value, "id"), incarnation, pid if pid is not None else "none")

    def scheduling(self, address, task):
        if address not in self.policies:
            pointer = number(task, "scheduling")
            self.policies[address] = self.read(pointer, "task_scheduling_t")
        return self.policies[address]

    def task_text(self, address):
        if not address:
            return "none"
        return self.task(address)[4]

    def matches(self, task, pid, selectors, slot=None):
        return (("--tid" not in selectors or number(task, "id") == selectors["--tid"])
                and ("--pid" not in selectors or pid == selectors["--pid"])
                and ("--slot" not in selectors or slot == selectors["--slot"]))

    def entries(self, field, type_name, capacity, selected=None):
        base = number(self.stats, field)
        if not base:
            raise ReadFailure("unavailable: null collection root")
        if selected is not None:
            if not 0 <= selected < capacity:
                raise ReadFailure("slot outside collection capacity")
            self.budget.take_entry()
            yield selected, self.read(base + selected * self.memory.sizeof(type_name), type_name)
            return
        # Fixed-capacity slot scans charge bytes even for empty slots. Only
        # occupied records consume entry budget; no unbounded discovery pass.
        active_field = "task" if type_name == SLEEP_TYPE else "used"
        stride = self.memory.sizeof(type_name)
        for index in range(capacity):
            address = base + index * stride
            if not int(self.memory.field(address, type_name, active_field, self.budget)):
                continue
            collection = readers.array(self.memory, address, type_name, 1, 1, self.budget)
            for issue in collection.issues:
                self.result.fail(issue)
            for item in collection.items:
                yield index, item["value"]



def check_cpu(stats, selectors):
    count = number(stats, "cpuCount")
    if number(stats, "cpuCapacity") != 8 or not 0 < count <= 8:
        raise ReadFailure("invalid configured CPU count/capacity")
    selected = selectors.get("--cpu")
    if selected is not None and selected >= count:
        raise ReadFailure("CPU outside configured inventory")
    return list(range(count)) if selected is None else [selected]


def eligibility(obs, task_address, task, process, cpu, sleeper):
    """Mirror stored scan exclusions, never execute accounting/retention/replenish."""
    reasons = []
    awakened = (number(sleeper, "task") == task_address and not bool(sleeper["waiting"])
                and not bool(sleeper["dispatched"]))
    state = number(task, "state")
    if state not in TASK_STATES:
        raise ReadFailure("invalid stored task state")
    if bool(task["dying"]) and not bool(task["sleepPending"]):
        reasons.append("dispatch-retention-unavailable")
    if state == 2:
        reasons.append("switching")
    if bool(task["dying"]) and not awakened:
        reasons.append("dying")
    if not bool(task["schedulerQueued"]) and not awakened:
        reasons.append("not-queued")
    if bool(task["sleepPending"]) and not awakened:
        reasons.append("sleep-pending")
    if not number(task, "affinityMask") & (1 << cpu):
        reasons.append("affinity")
    if number(task, "placementCpu") >= 0 and number(task, "placementCpu") != cpu:
        reasons.append("placement")
    if state == 1:
        running = int(obs.memory.field(obs.context.task_source, "TaskManager", "RunningTasks", obs.budget)[cpu])
        if running != task_address:
            reasons.append("running-elsewhere")
    if process is None:
        reasons.append("no-process")
    elif bool(process["stopped"]) and not bool(process["exited"]):
        reasons.append("process-stopped")
    attributes = obs.scheduling(task_address, task)
    effective = number(attributes, "effectivePolicy")
    if effective not in POLICIES:
        raise ReadFailure("unknown effective scheduling policy")
    if effective == 6:
        donor_address = number(attributes, "deadlineDonor") or task_address
        _, donor, _, _, _ = obs.task(donor_address)
        account = obs.scheduling(donor_address, donor)
        if not 0 <= number(account, "reservationCpu") < number(obs.stats, "cpuCount"):
            raise ReadFailure("invalid deadline reservation CPU")
        if not number(account, "deadlineRemaining"):
            reasons.append("budget-exhausted")
        if number(account, "reservationCpu") != cpu:
            reasons.append("reservation-cpu")
    elif effective in (1, 2):
        utilization = obs.scalar("deadlineUtilization", index=cpu)
        if utilization > 1000000:
            raise ReadFailure("invalid deadline utilization")
        capacity = (1000000 - utilization) * 95 * 100000000 // 100 // 1000000
        if obs.scalar("realtimeConsumed", index=cpu) >= capacity:
            reasons.append("realtime-throttled")
    return reasons


def scheduler(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="scheduler",
                    count_semantics="physical task/sleep slots and configured CPU slots; observed checks only",
                    units="time and budgets ns (monotonic); utilization ppm; service weighted units")
    try:
        stats = descriptor_root(memory, descriptor, "SchedulerDebugStats", 128, budget)
        if number(stats, "capacity") != 128 or number(stats, "sentinel") != NONE:
            raise ReadFailure("invalid scheduler capacity/sentinel")
        if selectors.get("--slot", 0) >= 128:
            raise ReadFailure("scheduler slot outside physical capacity")
        cpus = check_cpu(stats, selectors)
        obs = Observation(memory, stats, budget, result)
        if name == "cpus":
            if any(key in selectors for key in ("--pid", "--tid", "--slot")):
                raise ReadFailure("cpus accepts only --cpu")
            row(result, budget, "ready sequence", str(obs.scalar("readySequence")))
            for cpu in cpus:
                row(result, budget, "cpu %d" % cpu,
                    "active=%s idle=(%s) execution=(%s) stamp=%dns DLutil=%dppm RTwindow=%dns RTconsumed=%dns head=%s" % (
                        bool(obs.scalar("active", "bool", cpu)), obs.task_text(obs.scalar("idle", index=cpu)),
                        obs.task_text(obs.scalar("execution", index=cpu)), obs.scalar("executionStamp", index=cpu),
                        obs.scalar("deadlineUtilization", index=cpu), obs.scalar("realtimeWindow", index=cpu),
                        obs.scalar("realtimeConsumed", index=cpu), obs.scalar("heads", "size_t", cpu)))
        elif name == "tasks":
            for slot in range(128):
                if "--slot" in selectors and selectors["--slot"] != slot:
                    continue
                task = obs.context.read("task", slot)
                if task is None:
                    continue
                budget.take_entry()
                address = obs.context.address("task", slot)
                try:
                    _, task, pid, process, identity = obs.task(address)
                    if not obs.matches(task, pid, selectors, slot):
                        continue
                    attributes = obs.scheduling(address, task)
                    row(result, budget, identity,
                        "state=%s queued=%s affinity=%#x ownerCpu=%d placementCpu=%d dispatchUsers=%d sleepPending=%s blocked=%s; nice=%d/%d policy=%s/%s priority=%d/%d ready=%d RR=%dns DLremaining=%dns DLruntime=%dns period=%dns deadline=%dns release=%dns reservationCpu=%d donor=%#x service=%d" % (
                            TASK_STATES.get(number(task, "state"), "invalid"), bool(task["schedulerQueued"]),
                            number(task, "affinityMask"), number(task, "ownerCpu"), number(task, "placementCpu"), number(task, "dispatchUsers"),
                            bool(task["sleepPending"]), bool(task["sleepBlocked"]), number(task, "baseNice"), number(task, "effectiveNice"),
                            policy(number(attributes, "policy")), policy(number(attributes, "effectivePolicy")),
                            number(attributes, "priority"), number(attributes, "effectivePriority"), number(attributes, "readyOrder"),
                            number(attributes, "roundRobinRemaining"), number(attributes, "deadlineRemaining"), number(attributes, "deadlineRuntime"),
                            number(attributes, "deadlinePeriod"), number(attributes, "deadlineAbsolute"), number(attributes, "deadlineRelease"),
                            number(attributes, "reservationCpu"), number(attributes, "deadlineDonor"), number(task, "scheduleRuntime")))
                    sleeper = obs.read(number(stats, "sleepEntries") + slot * memory.sizeof(SLEEP_TYPE), SLEEP_TYPE)
                    for cpu in cpus:
                        reasons = eligibility(obs, address, task, process, cpu, sleeper)
                        answer = "unknown (partial dependencies)" if result.issues else "false" if reasons else "passes observed checks"
                        row(result, budget, "  cpu %d" % cpu, "eligible=%s reasons=%s; accounting is sampled, no next-task prediction" % (answer, ",".join(reasons) or "none observed"))
                except ReadFailure as error:
                    result.fail("task slot %d: %s; eligibility unknown" % (slot, error))
        elif name == "sleeps":
            reached = set()
            for cpu in range(number(stats, "cpuCount")):
                head = obs.scalar("heads", "size_t", cpu)
                row(result, budget, "cpu %d sleep head" % cpu, "none (sentinel=%d)" % NONE if head == NONE else str(head))
                chain = readers.index_list(memory, number(stats, "sleepEntries"), SLEEP_TYPE, head, 128, "next", NONE, budget)
                for issue in chain.issues:
                    result.fail("cpu %d: %s" % (cpu, issue))
                for item in chain.items:
                    slot, entry = item["index"], item["value"]
                    if slot in reached:
                        result.fail("sleep slot linked from multiple CPU heads")
                    reached.add(slot)
                    if not bool(entry["waiting"]) or number(entry, "cpuId") != cpu:
                        result.fail("sleep chain flags/CPU disagree with head")
            for slot, entry in obs.entries("sleepEntries", SLEEP_TYPE, 128, selectors.get("--slot")):
                pointer = number(entry, "task")
                if not pointer:
                    continue
                try:
                    task_slot, task, pid, process, identity = obs.task(pointer)
                    if task_slot != slot or number(entry, "process") != number(task, "process"):
                        raise ReadFailure("sleep slot/task/process mismatch")
                    if bool(entry["waiting"]) and slot not in reached:
                        result.fail("waiting sleep slot not reached from a CPU head")
                    if number(entry, "cpuId") >= number(stats, "cpuCount"):
                        raise ReadFailure("sleep CPU outside configured inventory")
                    if number(entry, "cpuId") not in cpus or not obs.matches(task, pid, selectors, slot):
                        continue
                    next_index = number(entry, "next")
                    if next_index != NONE and not 0 <= next_index < 128:
                        result.fail("sleep slot %d has invalid next index" % slot)
                    sequence_address = number(entry, "sequence")
                    sequence = None
                    if sequence_address:
                        try:
                            sequence = int(obs.read(sequence_address, "uint64_t"))
                        except ReadFailure as error:
                            result.fail("sleep event sequence: " + str(error))
                            sequence = "unreadable"
                    raw = number(entry, "result")
                    signed = raw - (1 << 64) if raw >= (1 << 63) else raw
                    row(result, budget, "sleep slot %d %s" % (slot, identity),
                        "cpu=%d next=%s linked=%s waiting=%s dispatched=%s uninterruptible=%s deadline=%dns monotonic result=%d sequence=%#x observed=%s expected=%d taskBlocked=%s" % (
                            number(entry, "cpuId"), "none" if number(entry, "next") == NONE else number(entry, "next"), slot in reached,
                            bool(entry["waiting"]), bool(entry["dispatched"]), bool(entry["uninterruptible"]), number(entry, "deadline"), signed,
                            sequence_address, sequence, number(entry, "expectedSequence"), bool(task["sleepBlocked"])))
                except ReadFailure as error:
                    result.fail("sleep slot %d: %s" % (slot, error))
        else:
            raise ReadFailure("unsupported scheduler view")
        obs.finish("Scheduler")
    except (ReadFailure, ValueError, OverflowError) as error:
        result.fail(error)
    return result


def key_text(entry, prefix=""):
    identity, backing, offset = (number(entry, prefix + field) for field in
                                 (("Identity", "Object", "Offset") if prefix else ("identity", "object", "offset")))
    if not identity or (backing and identity != backing):
        raise ReadFailure("invalid private/shared futex key")
    return "%s identity=%#x offset=%#x retainedBacking=%#x" % ("shared" if backing else "private", identity, offset, backing)


def sync(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="futex/PI",
                    count_semantics="physical used slots; queue is head/next order; PI edges observed only",
                    units="offset bytes (private: virtual address); sequences/order counters; wake bits mask")
    try:
        stats = descriptor_root(memory, descriptor, "SynchronizationDebugStats", 128, budget)
        if number(stats, "capacity") != 128 or number(stats, "sentinel") != NONE:
            raise ReadFailure("invalid synchronization bounds/sentinel")
        if selectors.get("--slot", 0) >= 128:
            raise ReadFailure("synchronization slot outside physical capacity")
        obs = Observation(memory, stats, budget, result)
        row(result, budget, "priority reschedule", "%#x; PI order=%d" % (obs.scalar("rescheduleMask"), obs.scalar("order")))
        if name == "waiters":
            head, tail = obs.scalar("head", "size_t"), obs.scalar("tail", "size_t")
            chain = readers.index_list(memory, number(stats, "waiters"), WAITER_TYPE, head, 128, "next", NONE, budget)
            for issue in chain.issues:
                result.fail(issue)
            linked = [item["index"] for item in chain.items]
            if not chain.issues and tail != (linked[-1] if linked else NONE):
                result.fail("futex tail disagrees with queue chain")
            row(result, budget, "queue", "head=%d tail=%d observed order=%s sentinel=%d" % (head, tail, linked, NONE))
            occupied = 0
            for slot, entry in obs.entries("waiters", WAITER_TYPE, 128, selectors.get("--slot")):
                if not bool(entry["used"]):
                    if slot in linked:
                        result.fail("unused futex slot linked in queue")
                    continue
                occupied += 1
                try:
                    _, task, pid, _, identity = obs.task(number(entry, "task"))
                    if bool(entry["queued"]) != (slot in linked):
                        result.fail("waiter queued flag differs from head reachability")
                    if not obs.matches(task, pid, selectors, slot):
                        continue
                    text = "%s next=%d queued=%s woken=%s requeuePriority=%s converted=%s sequence=%d bits=%#x; %s" % (
                        identity, number(entry, "next"), bool(entry["queued"]), bool(entry["woken"]), bool(entry["requeuePriority"]),
                        bool(entry["converted"]), number(entry, "sequence"), number(entry, "bits"), key_text(entry))
                    if bool(entry["requeuePriority"]):
                        text += "; destination " + key_text(entry, "destination")
                    row(result, budget, "waiter slot %d" % slot, text)
                except ReadFailure as error:
                    result.fail("waiter slot %d: %s" % (slot, error))
            if "--slot" not in selectors:
                row(result, budget, "waiter occupancy", "%d/128 used slots%s" % (
                    occupied, "; exhausted" if occupied == 128 else "; observed within reported bounds"))
        elif name == "pi":
            # Gather each bounded array once; no recursive target discovery.
            states = dict(obs.entries("states", STATE_TYPE, 128))
            edges = dict(obs.entries("piWaiters", PI_TYPE, 128))
            row(result, budget, "PI occupancy", "states=%d/128 edges=%d/128%s" % (
                len(states), len(edges), "; exhausted collection" if max(len(states), len(edges)) == 128 else "; observed within reported bounds"))
            owners, waiting = {}, {}
            for slot, state in states.items():
                if not bool(state["used"]):
                    continue
                try:
                    owner = number(state, "owner")
                    if owner:
                        owners[slot] = obs.task(owner)
                    else:
                        result.fail("PI state %d has no observed owner (dead/transitional)" % slot)
                    text = "dead=%s %s owner=(%s) memoryOwner=%#x address=%#x" % (
                        bool(state["dead"]), key_text(state), owners[slot][4] if slot in owners else "none",
                        number(state, "memoryOwner"), number(state, "address"))
                    if slot in owners:
                        owner_task = owners[slot][1]
                        attributes = obs.scheduling(owner, owner_task)
                        text += "; owner nice=%d/%d policy=%s/%s priority=%d/%d deadlineDonor=%#x" % (
                            number(owner_task, "baseNice"), number(owner_task, "effectiveNice"),
                            policy(number(attributes, "policy")), policy(number(attributes, "effectivePolicy")),
                            number(attributes, "priority"), number(attributes, "effectivePriority"), number(attributes, "deadlineDonor"))
                    memory_owner = number(state, "memoryOwner")
                    if memory_owner:
                        process_slot = obs.context.pointer_slot("process", memory_owner)
                        process = obs.context.read("process", process_slot)
                        if process is None:
                            raise ReadFailure("PI memory owner names an unallocated process")
                        text += "; memoryOwner pid=%d generation=%d" % (number(process, "id"), generation(process))
                    selected = selectors.get("--slot", slot) == slot
                    if "--tid" in selectors or "--pid" in selectors:
                        selected = slot in owners and obs.matches(owners[slot][1], owners[slot][2], selectors)
                        for edge in edges.values():
                            if bool(edge["used"]) and number(edge, "state") == slot:
                                _, task, pid, _, _ = obs.task(number(edge, "task"))
                                selected |= obs.matches(task, pid, selectors)
                    if selected:
                        row(result, budget, "PI state %d" % slot, text)
                except ReadFailure as error:
                    result.fail("PI state %d: %s" % (slot, error))
            for slot, edge in edges.items():
                if not bool(edge["used"]):
                    continue
                try:
                    state_slot = number(edge, "state")
                    if state_slot not in states or not bool(states[state_slot]["used"]):
                        raise ReadFailure("invalid PI state index or unused state")
                    pointer = number(edge, "task")
                    task_slot, task, pid, _, identity = obs.task(pointer)
                    if task_slot != slot:
                        raise ReadFailure("PI edge task disagrees with physical slot")
                    waiting[pointer] = number(states[state_slot], "owner")
                    if obs.matches(task, pid, {k: v for k, v in selectors.items() if k != "--slot"}) and selectors.get("--slot", state_slot) == state_slot:
                        attributes = obs.scheduling(pointer, task)
                        row(result, budget, "PI waiter slot %d" % slot,
                            "%s state=%d sequence=%d order=%d effective=%s/%d nice=%d donor=%#x owner=(%s)" % (
                                identity, state_slot, number(edge, "sequence"), number(edge, "order"),
                                policy(number(attributes, "effectivePolicy")), number(attributes, "effectivePriority"), number(task, "effectiveNice"),
                                number(attributes, "deadlineDonor"), owners[state_slot][4] if state_slot in owners else "invalid/unavailable"))
                except ReadFailure as error:
                    result.fail("PI edge slot %d: %s" % (slot, error))
            for start in waiting:
                seen, current = set(), start
                for _ in range(budget.depth):
                    budget.take_entry()
                    if current in seen:
                        result.fail("observed PI owner cycle at %#x; not proof of arbitrary lock deadlock" % current)
                        break
                    if current not in waiting:
                        break
                    seen.add(current)
                    current = waiting[current]
                else:
                    result.fail("truncated: PI correlation depth")
        else:
            raise ReadFailure("unsupported synchronization view")
        obs.finish("SynchronizationManager")
    except (ReadFailure, ValueError, OverflowError) as error:
        result.fail(error)
    return result


def time(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="clocks/timers",
                    count_semantics="configured clocks and allocated process slots; sampled RAM only",
                    units="deadlines/intervals/accounting ns; realtime base seconds; LAPIC ticks per 10ms")
    try:
        stats = descriptor_root(memory, descriptor, "TimeDebugStats", 64, budget)
        group(result, stats["group"], "timer manager")
        if name == "clocks":
            if any(key in selectors for key in ("--pid", "--tid", "--slot")):
                raise ReadFailure("clocks accepts only --cpu for calibration")
            row(result, budget, "manager clock", "available=%s realtime=%s resolution=%dns realtimeBase=%ds backend initialize=%#x read=%#x context=%#x; addresses only" % (
                bool(stats["clockAvailable"]), bool(stats["realtimeAvailable"]), number(stats, "resolutionNanoseconds"), number(stats, "realtimeSeconds"),
                number(stats, "backendInitialize"), number(stats, "backendRead"), number(stats, "backendContext")))
            samples = number(stats, "sampleReads")
            row(result, budget, "normal-path monotonic sample", "%s; successful reads=%d failures=%d; high-water ns, age unknown" % (
                str(number(stats, "sampleNanoseconds")) if samples else "unavailable", samples, number(stats, "sampleFailures")))
            pointer = number(stats, "platform")
            if not pointer:
                row(result, budget, "platform clock", "unavailable for custom backend")
            else:
                platform = memory.read(pointer, "arx_time_platform_debug_t", budget)
                group(result, platform["group"], "platform clock selection")
                source = number(platform, "source")
                source_id = number(platform, "sourceId")
                if source:
                    if source_id not in (1, 2):
                        raise ReadFailure("invalid selected clock identity")
                    clock = memory.read(source, "clock_source_t", budget)
                    if not number(clock, "numerator") or not number(clock, "denominator"):
                        raise ReadFailure("invalid clock scaling")
                    row(result, budget, "platform clock", "%s mask=%#x scaling=%d/%d ns per tick origin=%d elapsed=%d ticks nanoseconds=%d; independent samples, age unknown; context not followed" % (
                        {1: "HPET", 2: "ACPI PM timer"}[source_id], number(clock, "mask"), number(clock, "numerator"), number(clock, "denominator"),
                        number(clock, "origin"), number(clock, "elapsed"), number(clock, "nanoseconds")))
                else:
                    row(result, budget, "platform clock", "unavailable: no selected source")
                selected = selectors.get("--cpu")
                if selected is not None and not 0 <= selected < 8:
                    raise ReadFailure("calibration CPU outside physical capacity")
                for cpu in range(8) if selected is None else [selected]:
                    ticks = int(platform["lapicTicks"][cpu])
                    row(result, budget, "CPU %d LAPIC" % cpu, "%d ticks per 10ms" % ticks if ticks else "unavailable/unconfigured")
        elif name == "timers":
            if "--cpu" in selectors:
                raise ReadFailure("timers accepts process/task/slot selectors")
            obs = Observation(memory, stats, budget, result)
            target_pid = selectors.get("--pid", selectors.get("--slot"))
            if "--tid" in selectors:
                matched = []
                for _, task in obs.context.inventory("task"):
                    budget.take_entry()
                    if number(task, "id") == selectors["--tid"]:
                        pid, _ = obs.context.process_for_task(task)
                        matched.append(pid)
                if len(matched) != 1 or matched[0] is None:
                    raise ReadFailure("task selector has no unique process")
                target_pid = matched[0]
            if target_pid is not None and not 0 <= target_pid < 64:
                raise ReadFailure("timer process slot outside capacity")
            for slot in range(64) if target_pid is None else [target_pid]:
                process = obs.context.read("process", slot)
                if process is None:
                    continue
                budget.take_entry()
                incarnation = generation(process)
                group(result, process["timerDebugGroup"], "process timer")
                user, kernel = number(process, "userCpuNanoseconds"), number(process, "kernelCpuNanoseconds")
                row(result, budget, "pid=%d slot=%d generation=%d" % (number(process, "id"), slot, incarnation),
                    "real armed=%s expiredAwaitingRestart=%s deadline=%dns interval=%dns MONOTONIC; user deadline=%dns interval=%dns USER_CPU; prof deadline=%dns interval=%dns USER+KERNEL_CPU; charged user=%dns kernel=%dns total=%dns (excludes uncharged execution)" % (
                        bool(process["realTimerArmed"]), bool(process["realTimerExpired"]), number(process, "realTimerDeadline"), number(process, "realTimerInterval"),
                        int(process["cpuTimerDeadline"][0]), int(process["cpuTimerInterval"][0]), int(process["cpuTimerDeadline"][1]), int(process["cpuTimerInterval"][1]),
                        user, kernel, min(NONE, user + kernel)))
            obs.finish("TimeManager")
        else:
            raise ReadFailure("unsupported time view")
    except (ReadFailure, ValueError, OverflowError) as error:
        result.fail(error)
    return result


scheduler.selectors = ("--cpu", "--tid", "--pid", "--slot")
sync.selectors = ("--tid", "--pid", "--slot")
time.selectors = ("--cpu", "--tid", "--pid", "--slot")
