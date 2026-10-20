#!/usr/bin/env python3
"""Controlled process/task graphs and shared-resource corruption fixtures."""
import copy
from pathlib import Path
import shlex
import sys
import types
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gdb"))
sys.modules.setdefault("gdb", types.SimpleNamespace(Command=object, string_to_argv=shlex.split))
from framework import commands
from framework.results import Budget, ReadFailure, Result
from views import process as views


class Memory:
    def __init__(self):
        self.values = {}
        self.fields = {}

    def sizeof(self, name):
        return {"process_t": 1024, "task_t": 1024, "file_descriptor_t": 24,
                "uint32_t": 4}.get(name, 256)

    def read(self, address, name, budget):
        budget.take_bytes(self.sizeof(name))
        if (address, name) not in self.values:
            raise ReadFailure("unreadable fixture memory")
        return copy.deepcopy(self.values[address, name])

    def field(self, address, name, field, budget):
        budget.take_bytes(8)
        if (address, name, field) in self.fields:
            return copy.deepcopy(self.fields[address, name, field])
        if field == "allocated" and (address, name) not in self.values:
            return 0
        value = self.values.get((address, name), {})
        if field not in value:
            raise ReadFailure("missing fixture field: " + field)
        return copy.deepcopy(value[field])

    def field_address(self, address, name, field):
        if name == "ProcessManager" and field == "Processes":
            return 100000, None
        if name == "TaskManager" and field == "Tasks":
            return 200000, None
        if name == "process_group_list_t" and field == "ids":
            return address + 8, None
        raise ReadFailure("missing fixture field address")


def process(pid, parent=None):
    return {"allocated": 1, "id": pid, "hasParent": parent is not None, "parentId": parent or 0,
            "parentTaskId": 0, "groupId": 1, "sessionId": 1, "reclaiming": 0, "exited": 0,
            "retirementActive": 0, "descriptorTable": 300000, "filesystemContext": 400000,
            "fileLimit": {"current": 1024}, "supplementaryGroups": 0,
            "credentials": {"user": dict.fromkeys(("real", "effective", "saved", "filesystem"), 1000),
                            "group": dict.fromkeys(("real", "effective", "saved", "filesystem"), 1000)},
            "debug": {"generation": 1, "exhausted": 0, "published": 1, "parentId": parent or 0,
                      "parentTaskId": 0, "parentCaptured": 1, "parentGeneration": 1 if parent is not None else 0}}


def fixture():
    memory = Memory()
    stats = {"source": 10000, "capacity": 64, "tasks": 20000, "group": {"activeWriters": 0},
             "lifecycleGroup": 0, "credentialGroup": 0, "vfsRoute": 700000,
             "fileOperations": [0, 0, 0], "filePolls": [0, 0]}
    task_stats = {"source": 20000, "processes": 10000, "capacity": 128, "group": {"activeWriters": 0}}
    memory.values[100, "ProcessDebugStats"] = stats
    memory.values[200, "TaskDebugStats"] = task_stats
    memory.fields[10000, "ProcessManager", "DebugStats"] = stats
    memory.fields[20000, "TaskManager", "DebugStats"] = task_stats
    memory.values[100000, "process_t"] = process(0)
    memory.values[101024, "process_t"] = process(1, 0)
    memory.values[200000, "task_t"] = {"allocated": 1, "id": 17, "process": 100000,
        "descriptorTable": 300256, "filesystemContext": 400256,
        "debug": {"generation": 3, "exhausted": 0, "processGeneration": 1, "processAddress": 100000}}
    for pointer in (400000, 400256):
        memory.values[pointer, "filesystem_context_t"] = {"references": 2, "umask": 18, "cwdPath": b"/fixture\0"}
    for pointer, base in ((300000, 500000), (300256, 500256)):
        memory.values[pointer, "descriptor_table_t"] = {"references": 2, "count": 4, "entries": base}
        for index in range(4):
            memory.values[base + index * 24, "file_descriptor_t"] = {"file": 0, "operations": 0, "flags": 0}
    memory.values[500256 + 24, "file_descriptor_t"] = {"file": 600000, "operations": 123, "flags": 1}
    return memory


def descriptor(task=False):
    return {"root": 200 if task else 100, "rootSize": 256, "version": 1, "count": 0,
            "capacity": 128 if task else 64}


class Views(unittest.TestCase):
    def tree(self, memory=None, budget=None, selectors=None):
        return views.process("tree", memory or fixture(), descriptor(), budget or Budget(), selectors)

    def test_tree(self):
        result = self.tree()
        self.assertEqual(result.status, "complete")
        self.assertEqual(len(result.items), 2)
        self.assertIn("parent=PID 0 generation 1", str(result.items))

    def test_reused_parent_is_not_attached(self):
        memory = fixture()
        memory.values[100000, "process_t"]["debug"]["generation"] = 2
        result = self.tree(memory)
        self.assertEqual(result.status, "partial")
        self.assertIn("stale parent incarnation", str(result.issues))
        self.assertFalse(any(item["path"].startswith("  PID 1") for item in result.items))

    def test_missing_and_partially_published_parent(self):
        memory = fixture()
        del memory.values[100000, "process_t"]
        self.assertIn("parent missing", str(self.tree(memory).issues))
        memory = fixture()
        memory.values[101024, "process_t"]["debug"]["parentId"] = 2
        self.assertIn("publication incomplete", str(self.tree(memory).issues))

    def test_parent_cycle_and_depth(self):
        memory = fixture()
        memory.values[100000, "process_t"] = process(0, 1)
        self.assertIn("cycle", str(self.tree(memory).issues))
        self.assertIn("depth limit", str(self.tree(budget=Budget(depth=0)).issues))

    def test_identity_exhaustion_and_entry_limits(self):
        memory = fixture()
        memory.values[100000, "process_t"]["debug"]["exhausted"] = 1
        self.assertIn("exhausted", str(self.tree(memory).issues))
        self.assertEqual(self.tree(budget=Budget(entries=1)).status, "partial")
        self.assertEqual(self.tree(budget=Budget(bytes=256)).status, "partial")

    def test_byte_limit_keeps_already_read_tree_nodes(self):
        result = self.tree(budget=Budget(bytes=1800))
        self.assertEqual(result.status, "partial")
        self.assertIn("PID 0", str(result.items))
        self.assertIn("byte limit", str(result.issues))

    def test_task_override_and_opaque_route(self):
        result = views.task("files", fixture(), descriptor(True), Budget(), {"--tid": 17})
        self.assertEqual(result.status, "complete")
        self.assertIn("table=0x494e0 (process default=0x493e0)", str(result.items))
        self.assertIn("unverified/custom route", str(result.items))
        self.assertIn("CLOEXEC=True", str(result.items))

    def test_stale_task_process_link(self):
        memory = fixture()
        memory.values[200000, "task_t"]["debug"]["processGeneration"] = 9
        result = views.task("files", memory, descriptor(True), Budget(), {"--tid": 17})
        self.assertIn("stale or partially published", str(result.issues))

    def test_sparse_scan_is_independent_of_rows(self):
        memory = fixture()
        memory.values[300000, "descriptor_table_t"]["count"] = 1048576
        result = views.process("files", memory, descriptor(), Budget(), {"--pid": 0, "--scan": 2})
        self.assertEqual(result.status, "partial")
        self.assertIn("2/1048576 slots scanned", str(result.items))
        self.assertIn("slot scan ceiling", str(result.issues))

    def test_shared_file_decoded_once_with_verified_kind(self):
        memory = fixture()
        stats = memory.values[100, "ProcessDebugStats"]
        stats["fileOperations"][1] = 710000
        for index in (0, 1):
            memory.values[500000 + index * 24, "file_descriptor_t"] = {"file": 600000, "operations": 700000, "flags": index}
        memory.values[600000, "file_t"] = {
            "operations": 710000, "offset": 16, "statusFlags": 2, "refCount": 2,
            "inode": 0, "path": {"mount": 0, "dentry": 0}, "privateData": 0,
            "closeObserver": 0, "closeObserverContext": 0, "readinessObservers": 0}
        memory.fields[710000, "file_operations_t", "Poll"] = 0
        memory.values[101024, "process_t"]["descriptorTable"] = 300512
        memory.values[300512, "descriptor_table_t"] = {"references": 1, "count": 1, "entries": 500512}
        memory.values[500512, "file_descriptor_t"] = {"file": 600000, "operations": 700000, "flags": 0}
        result = views.process("files", memory, descriptor(), Budget(), {"--pid": 0})
        self.assertEqual(result.status, "complete")
        self.assertEqual(sum(item["path"] == "Open file" for item in result.items), 1)
        self.assertIn("kind=memory file", str(result.items))
        self.assertIn("Observed file owner", str(result.items))
        self.assertIn("process slot=1 generation=1", str(result.items))
        self.assertEqual(sum(item["path"].startswith("FD ") for item in result.items), 2)

    def test_duplicate_tid_is_ambiguous(self):
        memory = fixture()
        memory.values[201024, "task_t"] = copy.deepcopy(memory.values[200000, "task_t"])
        result = views.task("files", memory, descriptor(True), Budget(), {"--tid": 17})
        self.assertIn("ambiguous", str(result.issues))

    def test_invalid_descriptor_capacity(self):
        memory = fixture()
        memory.values[300000, "descriptor_table_t"]["count"] = 1048577
        result = views.process("files", memory, descriptor(), Budget(), {"--pid": 0})
        self.assertIn("invalid descriptor", str(result.issues))

    def test_credentials_and_bounded_group_count(self):
        memory = fixture()
        result = views.process("credentials", memory, descriptor(), Budget(), {"--pid": 0})
        self.assertEqual(result.status, "complete")
        self.assertIn("filesystem=1000", str(result.items))
        memory.values[100000, "process_t"]["supplementaryGroups"] = 800000
        memory.fields[800000, "process_group_list_t", "count"] = 65537
        memory.fields[800000, "process_group_list_t", "references"] = 1
        self.assertIn("invalid supplementary", str(views.process("credentials", memory, descriptor(), Budget(), {"--pid": 0}).issues))

    def test_weak_observer_cycle(self):
        memory = fixture()
        context = views.Context(memory, memory.values[100, "ProcessDebugStats"],
                                memory.values[200, "TaskDebugStats"], Budget(), Result())
        file = {"closeObserver": 1, "closeObserverContext": 2, "readinessObservers": 900000}
        memory.values[900000, "file_close_observer_t"] = {"next": 900000, "Closing": 3, "context": 4}
        with self.assertRaisesRegex(ReadFailure, "cycle"):
            views.observers(context, file)

    def test_interrupted_group(self):
        memory = fixture()
        memory.values[100, "ProcessDebugStats"]["group"]["activeWriters"] = 2
        self.assertIn("interrupted", str(self.tree(memory).issues))

    def test_parser_limits_and_selector_applicability(self):
        words, _, selectors = commands.parse("task files --tid 0 --scan 32")
        self.assertEqual(words, ["task", "files"])
        self.assertEqual(selectors, {"--tid": 0, "--scan": 32})
        for text in ("task files --tid f()", "task files --pid 1 --tid 2", "process files --scan 1048577"):
            with self.assertRaises(ReadFailure):
                commands.parse(text)
        self.assertIn("only supported by file", str(self.tree(selectors={"--scan": 32}).issues))


if __name__ == "__main__":
    unittest.main()
