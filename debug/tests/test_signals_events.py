"""Stopped-memory contracts for signal queues, weak epolls and notification rings."""
import unittest
import importlib
import shlex
import sys
from types import SimpleNamespace
from unittest.mock import patch

from test_scheduling import Fixture as IdentityFixture, marker
from framework.results import Budget, ReadFailure
from views import signals_events as view


def text(result):
    return "\n".join(item.get("path", "") + " " + item["text"] for item in result.items)


class Fixture(IdentityFixture):
    def __init__(self, events=False):
        super().__init__("EventDebugStats" if events else "SignalDebugStats",
                         "EventManager" if events else "SignalManager", 0)
        self.stats.update(processes=0x2000, signals=0, counterOperations=201, epollOperations=202,
                          inotifyOperations=203, signalOperations=204, watchClosing=301,
                          epollClosing=302, readinessLock=0x65000)
        self.memory.records[0x65000, "spinlock_t"] = 0
        self.memory.records[0x2000, "ProcessDebugStats"].update(vfsRoute=100)
        self.process.update(descriptorTable=0x70000, signalPending=0, signalQueue=0,
                            signalDispositions=0x66000, resourceLock=0, stopSignal=0,
                            stopSequence=1, terminationSignal=0, exitRecipient=0)
        self.task_value = self.memory.records[0x20000, "task_t"]
        self.task_value.update(descriptorTable=0, signalPending=0, signalQueue=0,
                               signalSenders=0, signalMask=0, signalSavedMask=0,
                               signalRestoreMask=False, signalDepth=0, signalSequence=1,
                               signalStack=dict(sp=0, size=0, flags=2))
        self.memory.arrays[0x20000, "task_t", "signalSequence"] = 0x20080
        self.memory.arrays[0x10000, "process_t", "signalSenders"] = 0x68000
        self.dispositions = dict(references=2, lock=0, ignored=0,
                                 actions=[dict(handler=0, flags=0, restorer=0, mask=0)
                                          for _ in range(64)])
        self.memory.records[0x66000, "signal_dispositions_t"] = self.dispositions
        self.memory.records[0x70000, "descriptor_table_t"] = dict(count=2, entries=0x71000,
                                                                  references=2, lock=0)
        for index in range(2):
            self.memory.records[0x71000 + index * 256, "file_descriptor_t"] = dict(file=0, operations=100)
        self.memory.fields.update({(0x1000, "EventManager", "Epolls"): 0,
                                   (0x1000, "EventManager", "Notifications"): 0,
                                   (0x1000, "EventManager", "NotificationLock"): 0})
        for field in ("Generation", "TopologyGeneration", "NotificationGeneration", "ExclusiveGeneration"):
            self.memory.fields[0x1000, "EventManager", field] = 1
        self.memory.arrays[0x1000, "EventManager", "Generation"] = 0x1100
        self.reads = []
        original = self.memory.read

        def read(address, name, budget):
            self.reads.append((address, name))
            return original(address, name, budget)

        self.memory.read = read

    def file(self, pointer, private, operations, kind, fd=None):
        inode = private + (64 if kind == "event_epoll_t" else 0)
        self.memory.arrays[private, kind, "inode"] = inode
        self.memory.fields[inode, "inode_t", "type"] = 0
        value = dict(refCount=1, operations=operations, privateData=private,
                     inode=inode, ioLock=0, refLock=0, readinessObservers=0)
        self.memory.records[pointer, "file_t"] = value
        if fd is not None:
            self.memory.records[0x71000 + fd * 256, "file_descriptor_t"]["file"] = pointer
        return value

    def queue(self, pointer, owner, records, reserved=0):
        owner["signalQueue"] = pointer
        self.memory.fields[pointer, "signal_queue_t", "count"] = len(records)
        self.memory.fields[pointer, "signal_queue_t", "reserved"] = reserved
        self.memory.arrays[pointer, "signal_queue_t", "records"] = pointer + 256
        for index, signal in enumerate(records):
            owner["signalPending"] |= 1 << (signal - 1)
            self.memory.records[pointer + 256 + index * 256, "signal_queue_t::record_t"] = dict(
                number=signal, sender=dict(pid=4, uid=5, code=-1, value=index))

    def epoll(self, pointer=0x80000, file_pointer=0x90000, fd=0):
        file = self.file(file_pointer, pointer, 202, "event_epoll_t", fd)
        epoll = dict(manager=0x1000, file=file_pointer, next=0, head=0, tail=0,
                     sequence=3, topologyGeneration=1, topologyDepth=1, notificationGeneration=1)
        self.memory.records[pointer, "event_epoll_t"] = epoll
        self.memory.arrays[pointer, "event_epoll_t", "observer"] = pointer
        self.memory.arrays[pointer, "event_epoll_t", "sequence"] = pointer + 32
        self.memory.records[pointer, "file_close_observer_t"] = dict(context=pointer, Closing=302, next=0)
        file["readinessObservers"] = pointer
        self.memory.fields[0x1000, "EventManager", "Epolls"] = pointer
        return epoll, file

    def watch(self, owner, pointer, file_pointer):
        owner_pointer = next(address for (address, kind), value in self.memory.records.items()
                             if kind == "event_epoll_t" and value is owner)
        watch = dict(owner=owner_pointer, file=file_pointer, descriptor=1,
                     event=dict(events=0xD0000001, data=42), armed=False, pending=True, next=0)
        self.memory.records[pointer, "event_epoll_watch_t"] = watch
        self.memory.arrays[pointer, "event_epoll_watch_t", "observer"] = pointer
        file = self.memory.records[file_pointer, "file_t"]
        self.memory.records[pointer, "file_close_observer_t"] = dict(
            context=pointer, Closing=301, next=file["readinessObservers"])
        file["readinessObservers"] = pointer
        owner.update(head=pointer, tail=pointer)
        return watch

    def notification(self, kind=1):
        pointer = 0xA0000
        self.file(0xB0000, pointer, 203 if kind == 1 else 204, "event_notification_t", 0)
        common = dict(manager=0x1000, file=0xB0000, next=0, kind=kind)
        self.memory.records[pointer, "event_notification_t"] = common
        self.memory.fields[0x1000, "EventManager", "Notifications"] = pointer
        if kind == 2:
            self.memory.fields[pointer, "signal_descriptor_t", "mask"] = 1 << 31
            return common
        self.memory.fields[pointer, "inotify_state_t", "head"] = 63
        self.memory.fields[pointer, "inotify_state_t", "count"] = 2
        self.memory.arrays[pointer, "inotify_state_t", "watches"] = 0xC0000
        self.memory.arrays[pointer, "inotify_state_t", "records"] = 0xD0000
        for index in range(128):
            self.memory.records[0xC0000 + index * 256, "notification_watch_t"] = dict(
                inode=0xF0000 if index == 127 else 0, descriptor=7, mask=0x80000200)
        for physical, mask, name in ((63, 0x8000, b"a\0\0\0"), (0, 0x4000, b"")):
            self.memory.records[0xD0000 + physical * 256, "notification_record_t"] = dict(
                descriptor=7, mask=mask, cookie=3, length=len(name), name=list(name))
        return common

    def gather(self, name, selectors=None, limit=1000, depth=8, byte_limit=1024 * 1024):
        adapter = view.events if self.root_type == "EventDebugStats" else view.signals
        return adapter(name, self.memory, self.descriptor, Budget(limit, byte_limit, depth), selectors)


class SignalViews(unittest.TestCase):
    def test_process_and_task_masks_senders_reservations_and_restoration(self):
        fixture = Fixture()
        fixture.queue(0xA0000, fixture.task_value, [32, 32, 64], reserved=2)
        fixture.task_value.update(signalMask=1 << 31, signalSavedMask=1 << 63,
                                  signalRestoreMask=True, signalDepth=2)
        fixture.process["signalPending"] = 1 << 16
        fixture.memory.records[0x68000 + 16 * 256, "signal_sender_t"] = dict(pid=7, uid=8, code=1, value=9)
        result = fixture.gather("pending", {"--tid": 9})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("process-directed CHLD sender pid=7", text(result))
        self.assertIn("count=3 reserved=2", text(result))
        self.assertIn("restore=True depth=2", text(result))
        self.assertIn("eligible=0x8000000000010000", text(result))
        self.assertIn("task-directed queue[1] RTMIN+0", text(result))

    def test_invalid_queue_and_missing_records_do_not_read_outside_capacity(self):
        fixture = Fixture()
        fixture.queue(0xA0000, fixture.process, [32], reserved=128)
        result = fixture.gather("pending", {"--pid": 0})
        self.assertIn("invalid signal queue count/reserved", " ".join(result.issues))
        self.assertFalse(any(kind == "signal_queue_t::record_t" for _, kind in fixture.reads))
        fixture.memory.fields[0xA0000, "signal_queue_t", "reserved"] = 0
        fixture.memory.fields[0xA0000, "signal_queue_t", "count"] = 0
        self.assertIn("RT pending bits differ", " ".join(fixture.gather("pending").issues))

    def test_disposition_identity_and_actions(self):
        fixture = Fixture()
        fixture.dispositions["actions"][9].update(handler=0x1234, mask=1 << 31, restorer=0x5678)
        result = fixture.gather("dispositions", {"--pid": 0})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("USR1 handler=0x1234", text(result))
        self.assertIn("references=2", text(result))
        fixture.dispositions["references"] = 0
        self.assertIn("no retained references", " ".join(fixture.gather("dispositions").issues))

    def test_interrupted_state_suppresses_eligibility_and_stale_task_is_rejected(self):
        fixture = Fixture()
        fixture.dispositions["lock"] = 1
        result = fixture.gather("pending", {"--tid": 9})
        self.assertIn("eligible=unknown", text(result))
        fixture.task_value["debug"]["processGeneration"] = 99
        self.assertIn("stale", " ".join(fixture.gather("pending").issues))

    def test_null_queue_sender_and_limits(self):
        fixture = Fixture()
        self.assertIn("null/unpublished", text(fixture.gather("pending")))
        fixture.task_value["signalPending"] = 1
        self.assertIn("sender storage", " ".join(fixture.gather("pending").issues))
        self.assertEqual(fixture.gather("pending", limit=1).status, "partial")
        self.assertEqual(fixture.gather("pending", byte_limit=256).status, "partial")
        self.assertIn("mutually exclusive", " ".join(fixture.gather("pending", {"--pid": 0, "--tid": 9}).issues))


class CommandGrammar(unittest.TestCase):
    def test_registered_views_preserve_provider_grammar_and_numeric_selection(self):
        fake_gdb = SimpleNamespace(Command=object, string_to_argv=shlex.split)
        with patch.dict(sys.modules, {"gdb": fake_gdb}):
            commands = importlib.import_module("framework.commands")
            for provider, names in (("signals", ("pending", "dispositions")),
                                    ("events", ("epoll", "notifications", "counter"))):
                semantic = "arx." + provider
                self.assertIn((semantic, 1), commands.ADAPTERS)
                for name in names:
                    words, _, selectors = commands.parse(provider + " " + name + " --tid 9")
                    self.assertEqual(words, [provider, name])
                    self.assertEqual(selectors, {"--tid": 9})
            with self.assertRaises(ReadFailure):
                commands.parse("signals pending --pid 0 --tid 9")
            with self.assertRaises(ReadFailure):
                commands.parse("events counter --tid 9 --fd function()")


class EventViews(unittest.TestCase):
    def test_wrapped_inotify_order_overflow_ignored_and_physical_watch_slots(self):
        fixture = Fixture(True)
        fixture.notification()
        fixture.stats["processes"] = 0  # Global objects do not require identity adapters.
        result = fixture.gather("notifications")
        self.assertEqual(result.status, "complete", result.issues)
        output = text(result)
        self.assertLess(output.index("record[0] physical=63"), output.index("record[1] physical=0"))
        self.assertIn("watch slot=127", output)
        self.assertIn("IGNORED", output)
        self.assertIn("OVERFLOW", output)
        self.assertIn("overflow True", output)
        self.assertFalse(any(kind == "inotify_state_t" for _, kind in fixture.reads))

    def test_invalid_ring_bounds_and_unterminated_name(self):
        fixture = Fixture(True)
        fixture.notification()
        fixture.memory.fields[0xA0000, "inotify_state_t", "count"] = 65
        self.assertIn("invalid inotify ring", " ".join(fixture.gather("notifications").issues))
        self.assertFalse(any(kind == "notification_record_t" for _, kind in fixture.reads))
        fixture.memory.fields[0xA0000, "inotify_state_t", "count"] = 1
        fixture.memory.records[0xD0000 + 63 * 256, "notification_record_t"]["name"] = [65] * 4
        self.assertIn("unterminated", " ".join(fixture.gather("notifications").issues))

    def test_notification_identity_rejected_before_private_state(self):
        fixture = Fixture(True)
        fixture.notification()
        fixture.memory.records[0xB0000, "file_t"]["operations"] = 999
        result = fixture.gather("notifications")
        self.assertIn("identity mismatch", " ".join(result.issues))
        self.assertFalse(any(kind == "notification_record_t" for _, kind in fixture.reads))

    def test_wrong_selected_descriptor_reports_opaque_type(self):
        fixture = Fixture(True)
        fixture.file(0x90000, 0x80000, 201, "event_counter_t", 0)
        result = fixture.gather("notifications", {"--pid": 0, "--fd": 0})
        self.assertIn("no matching verified event type", " ".join(result.issues))

    def test_signalfd_matches_selected_context_including_blocked_signal(self):
        fixture = Fixture(True)
        fixture.notification(2)
        fixture.task_value.update(signalPending=1 << 31, signalMask=1 << 31)
        result = fixture.gather("notifications", {"--tid": 9, "--fd": 0})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("taskMatch=0x80000000", text(result))
        self.assertIn("caller mask not applied", text(result))
        fixture.stats["signals"] = 0x67000
        fixture.memory.records[0x67000, "SignalDebugStats"] = dict(group=marker(1))
        result = fixture.gather("notifications", {"--tid": 9, "--fd": 0})
        self.assertIn("signal manager: operation interrupted", " ".join(result.issues))
        self.assertIn("no intrinsic recipient", text(fixture.gather("notifications")))

    def test_descriptor_counter_value_semaphore_and_lock(self):
        fixture = Fixture(True)
        fixture.file(0x90000, 0x80000, 201, "event_counter_t", 0)
        fixture.memory.records[0x80000, "event_counter_t"] = dict(manager=0x1000, value=2, semaphore=True, lock=0)
        result = fixture.gather("counter", {"--tid": 9, "--fd": 0})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("value=2 semaphore=True", text(result))
        fixture.memory.records[0x80000, "event_counter_t"]["lock"] = 1
        self.assertIn("lock held", " ".join(fixture.gather("counter").issues))
        fixture.memory.records[0x90000, "file_t"]["operations"] = 999
        fixture.reads.clear()
        self.assertIn("privateData remains opaque", " ".join(fixture.gather("counter", {"--pid": 0, "--fd": 0}).issues))
        self.assertFalse(any(kind == "event_counter_t" for _, kind in fixture.reads))

    def test_epoll_stored_flags_and_weak_source(self):
        fixture = Fixture(True)
        epoll, _ = fixture.epoll()
        fixture.file(0x91000, 0x81000, 999, "event_counter_t", 1)
        fixture.watch(epoll, 0x82000, 0x91000)
        result = fixture.gather("epoll", {"--pid": 0, "--fd": 0})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("EXCLUSIVE|ONESHOT|ET", text(result))
        self.assertIn("armed=False pending=True", text(result))
        fixture.memory.records[0x91000, "file_t"]["readinessObservers"] = 0
        self.assertIn("stale weak file", " ".join(fixture.gather("epoll").issues))

    def test_nested_epolls_cycles_depth_and_retired_source(self):
        fixture = Fixture(True)
        parent, _ = fixture.epoll()
        child, _ = fixture.epoll(0x83000, 0x93000, 1)
        child["next"] = 0x80000
        fixture.watch(parent, 0x82000, 0x93000)
        result = fixture.gather("epoll", {"--pid": 0, "--fd": 0})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("epoll=0x83000", text(result))
        self.assertIn("nested epoll depth", " ".join(fixture.gather("epoll", depth=1).issues))
        fixture.watch(child, 0x84000, 0x90000)
        self.assertIn("cycle in nested", " ".join(fixture.gather("epoll").issues))
        fixture.memory.records[0x90000, "file_t"]["refCount"] = 0
        self.assertIn("retiring weak file", " ".join(fixture.gather("epoll").issues))

    def test_list_cycles_unpublished_groups_and_scan_limits(self):
        fixture = Fixture(True)
        notification = fixture.notification(2)
        notification["next"] = 0xA0000
        self.assertIn("cycle", " ".join(fixture.gather("notifications").issues))
        notification["next"] = 0
        fixture.stats["group"] = marker(1)
        fixture.memory.fields[0x1000, "EventManager", "NotificationLock"] = 1
        result = fixture.gather("notifications")
        self.assertIn("operation interrupted", " ".join(result.issues))
        self.assertIn("lock held", " ".join(result.issues))
        fixture.notification()
        self.assertIn("scan ceiling", " ".join(fixture.gather("notifications", {"--scan": 1}).issues))
        self.assertEqual(fixture.gather("notifications", limit=1).status, "partial")
        fixture.stats["source"] = 0
        self.assertIn("unpublished", " ".join(fixture.gather("notifications").issues))


if __name__ == "__main__":
    unittest.main()
