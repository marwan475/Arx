"""Focused adapter checks using stopped-memory fixtures, without GDB or a kernel."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gdb"))
from framework.results import Budget, ReadFailure
from views import scheduling as view


def marker(active=0):
    return dict(activeWriters=active, completedUpdates=0)


class Memory:
    def __init__(self):
        self.records = {}
        self.fields = {}
        self.arrays = {}

    def sizeof(self, name):
        return 1 if name == "bool" else 8 if name in ("uint64_t", "size_t", "task_t*") else 256

    def read(self, address, name, budget):
        budget.take_bytes(self.sizeof(name))
        if (address, name) not in self.records:
            raise ReadFailure("unreadable fixture pointer %#x (%s)" % (address, name))
        return self.records[address, name]

    def field(self, address, name, field, budget):
        budget.take_bytes(8)
        if (address, name, field) in self.fields:
            return self.fields[address, name, field]
        if (address, name) in self.records:
            return self.records[address, name][field]
        if field in ("allocated", "used", "task"):
            return 0
        raise ReadFailure("missing fixture field " + field)

    def field_address(self, address, name, field):
        return self.arrays[address, name, field], None


class Fixture:
    def __init__(self, root_type="SchedulerDebugStats", source_type="Scheduler", capacity=128):
        self.memory = Memory()
        self.root_type, self.source_type = root_type, source_type
        self.stats = dict(source=0x1000, processStats=0x2000, taskStats=0x3000,
                          group=marker(), capacity=capacity, sentinel=view.NONE,
                          cpuCount=2, cpuCapacity=8)
        self.descriptor = dict(root=0x4000, rootSize=256, count=0, capacity=capacity, version=1)
        processes = dict(source=0x5000, tasks=0x6000, taskStats=0x3000, capacity=64,
                         group=marker(), lifecycleGroup=0, credentialGroup=0)
        tasks = dict(source=0x6000, capacity=128, group=marker())
        self.memory.records.update({(0x4000, root_type): self.stats,
                                    (0x2000, "ProcessDebugStats"): processes,
                                    (0x3000, "TaskDebugStats"): tasks})
        self.memory.fields[0x1000, source_type, "DebugStats"] = self.stats
        self.memory.arrays[0x5000, "ProcessManager", "Processes"] = 0x10000
        self.memory.arrays[0x6000, "TaskManager", "Tasks"] = 0x20000
        self.memory.fields[0x6000, "TaskManager", "RunningTasks"] = [0x20000, 0] + [0] * 6
        self.process = dict(allocated=True, id=0, debug=dict(generation=2, exhausted=False), stopped=False,
                            exited=False, timerDebugGroup=marker(), realTimerArmed=True, realTimerExpired=False,
                            realTimerDeadline=5000, realTimerInterval=1000, cpuTimerDeadline=[100, 200],
                            cpuTimerInterval=[10, 20], userCpuNanoseconds=50, kernelCpuNanoseconds=25)
        self.memory.records[0x10000, "process_t"] = self.process
        self.task(0, 9)
        self.base = 0x40000

    def root(self, field, type_name, values):
        self.base += 0x10000
        self.stats[field] = self.base
        if not isinstance(values, list):
            values = [values]
        for index, value in enumerate(values):
            self.memory.records[self.base + index * self.memory.sizeof(type_name), type_name] = value
        return self.base

    def task(self, slot, tid):
        address = 0x20000 + slot * 256
        state = dict(policy=0, effectivePolicy=0, priority=0, effectivePriority=0,
                     readyOrder=1, roundRobinRemaining=100, deadlineRemaining=10, deadlineRuntime=20,
                     deadlinePeriod=100, deadlineAbsolute=200, deadlineRelease=100,
                     reservationCpu=0, deadlineDonor=0)
        task = dict(allocated=True, id=tid, debug=dict(generation=3, exhausted=False,
                    processGeneration=2, processAddress=0x10000), process=0x10000,
                    state=0, dying=False, schedulerQueued=True, sleepPending=False, sleepBlocked=False,
                    affinityMask=1, placementCpu=0, ownerCpu=-1, dispatchUsers=0,
                    baseNice=0, effectiveNice=0, scheduling=0x30000 + slot * 256, scheduleRuntime=1)
        self.memory.records[address, "task_t"] = task
        self.memory.records[task["scheduling"], "task_scheduling_t"] = state
        return address, task, state

    def sleeps(self, next_index=view.NONE):
        entry = dict(task=0x20000, process=0x10000, deadline=5000, result=0, sequence=0,
                     expectedSequence=2, next=next_index, cpuId=0, waiting=True,
                     dispatched=False, uninterruptible=False)
        self.root("sleepEntries", view.SLEEP_TYPE, [entry])
        self.root("heads", "size_t", [0, view.NONE])
        return entry

    def gather(self, name, selectors=None, limit=1000):
        adapter = {"SchedulerDebugStats": view.scheduler, "SynchronizationDebugStats": view.sync,
                   "TimeDebugStats": view.time}[self.root_type]
        return adapter(name, self.memory, self.descriptor, Budget(limit, 1024 * 1024, 8), selectors)


def text(result):
    return "\n".join(item.get("path", "") + " " + item["text"] for item in result.items)


class SchedulingViews(unittest.TestCase):
    def test_task_identity_and_affinity_reasons(self):
        fixture = Fixture()
        fixture.sleeps()
        result = fixture.gather("tasks", {"--tid": 9, "--cpu": 1})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("slot=0 tid=9 generation=3", text(result))
        self.assertIn("affinity,placement", text(result))

    def test_deadline_donor_budget_and_rr_throttle(self):
        fixture = Fixture()
        fixture.sleeps()
        _, task, state = fixture.task(0, 9)
        _, _, donor = fixture.task(1, 7)
        state.update(effectivePolicy=6, deadlineDonor=0x20100)
        donor["deadlineRemaining"] = 0
        self.assertIn("budget-exhausted", text(fixture.gather("tasks", {"--tid": 9, "--cpu": 0})))
        state.update(effectivePolicy=2, deadlineDonor=0)
        fixture.root("deadlineUtilization", "uint64_t", [0, 0])
        fixture.root("realtimeConsumed", "uint64_t", [95000000, 0])
        self.assertIn("realtime-throttled", text(fixture.gather("tasks", {"--tid": 9, "--cpu": 0})))

    def test_sleep_slot_is_not_tid_and_cycles_are_bounded(self):
        fixture = Fixture()
        entry = fixture.sleeps(0)
        result = fixture.gather("sleeps", {"--slot": 0})
        self.assertTrue(any("cycle" in issue for issue in result.issues))
        self.assertIn("tid=9", text(result))
        entry["next"] = 999
        result = fixture.gather("sleeps", {"--slot": 0})
        self.assertTrue(any("invalid index" in issue for issue in result.issues))

    def test_partial_group_invalid_identity_and_truncation(self):
        fixture = Fixture()
        fixture.sleeps()
        fixture.stats["group"]["activeWriters"] = 2
        self.assertIn("eligible=unknown", text(fixture.gather("tasks", {"--tid": 9, "--cpu": 0})))
        fixture.memory.records[0x20000, "task_t"]["debug"]["processGeneration"] = 1
        self.assertEqual(fixture.gather("tasks").status, "partial")
        self.assertEqual(fixture.gather("sleeps", limit=1).status, "partial")
        self.assertTrue(any("outside physical capacity" in issue for issue in
                            fixture.gather("tasks", {"--slot": 128}).issues))

    def sync_fixture(self):
        fixture = Fixture("SynchronizationDebugStats", "SynchronizationManager")
        fixture.root("head", "size_t", view.NONE)
        fixture.root("tail", "size_t", view.NONE)
        fixture.root("order", "uint64_t", 1)
        fixture.root("rescheduleMask", "uint64_t", 3)
        return fixture

    def test_private_and_shared_waiter_keys(self):
        fixture = self.sync_fixture()
        entry = dict(used=True, task=0x20000, identity=0x8000, object=0, offset=0x1000,
                     next=view.NONE, queued=True, woken=False, requeuePriority=True, converted=False,
                     sequence=4, bits=0xFF, destinationIdentity=0x9000,
                     destinationObject=0x9000, destinationOffset=4096)
        fixture.root("waiters", view.WAITER_TYPE, [entry])
        fixture.memory.records[fixture.stats["head"], "size_t"] = 0
        fixture.memory.records[fixture.stats["tail"], "size_t"] = 0
        result = fixture.gather("waiters", {"--tid": 9})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("private identity=0x8000", text(result))
        self.assertIn("destination shared identity=0x9000", text(result))

    def test_pi_cycle_invalid_state_and_owner(self):
        fixture = self.sync_fixture()
        fixture.task(1, 7)
        state = dict(used=True, dead=False, identity=0x8000, object=0, offset=4096,
                     owner=0x20100, memoryOwner=0x10000, address=4096)
        second = dict(state, owner=0x20000, offset=8192)
        fixture.root("states", view.STATE_TYPE, [state, second])
        edge = dict(used=True, state=0, task=0x20000, sequence=1, order=1)
        other = dict(edge, state=1, task=0x20100)
        fixture.root("piWaiters", view.PI_TYPE, [edge, other])
        result = fixture.gather("pi")
        self.assertTrue(any("observed PI owner cycle" in issue for issue in result.issues), result.issues)
        edge["state"] = 999
        state["owner"] = 0xBAD
        result = fixture.gather("pi")
        self.assertTrue(any("invalid PI state index" in issue for issue in result.issues))
        self.assertTrue(any("not a slot address" in issue for issue in result.issues))

    def test_unavailable_custom_clock_and_timer_domains(self):
        fixture = Fixture("TimeDebugStats", "TimeManager", 64)
        fixture.stats.update(clockAvailable=0, realtimeAvailable=0, realtimeSeconds=0,
                             resolutionNanoseconds=0, backendInitialize=0xBAD, backendRead=0xBAD,
                             backendContext=0xBAD, sampleReads=0, sampleFailures=1,
                             sampleNanoseconds=0, platform=0)
        result = fixture.gather("clocks")
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("unavailable", text(result))
        result = fixture.gather("timers", {"--pid": 0})
        self.assertEqual(result.status, "complete", result.issues)
        for domain in ("MONOTONIC", "USER_CPU", "USER+KERNEL_CPU"):
            self.assertIn(domain, text(result))

    def test_platform_clock_reads_ram_not_callbacks(self):
        fixture = Fixture("TimeDebugStats", "TimeManager", 64)
        fixture.stats.update(clockAvailable=1, realtimeAvailable=1, realtimeSeconds=10,
                             resolutionNanoseconds=1, backendInitialize=0xBAD, backendRead=0xBAD,
                             backendContext=0xBAD, sampleReads=2, sampleFailures=0,
                             sampleNanoseconds=50, platform=0x80000)
        fixture.memory.records[0x80000, "arx_time_platform_debug_t"] = dict(
            source=0x90000, sourceId=2, group=marker(), lapicTicks=[3] + [0] * 7)
        fixture.memory.records[0x90000, "clock_source_t"] = dict(
            mask=0xFFFFFF, numerator=1000000000, denominator=3579545,
            origin=1, elapsed=200, nanoseconds=500, read=0xBAD, context=0xBAD)
        result = fixture.gather("clocks", {"--cpu": 0})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("ACPI PM timer", text(result))
        self.assertIn("3 ticks per 10ms", text(result))


if __name__ == "__main__":
    unittest.main()
