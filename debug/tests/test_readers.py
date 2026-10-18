"""Meaningful corruption and work-bound checks without a running target."""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "gdb"))
from framework import readers
from framework.results import Budget, ReadFailure


class Memory:
    def __init__(self):
        self.nodes = {16: {"next": 32, "index": 1},
                      32: {"next": 0, "index": -1}}

    def sizeof(self, name):
        return 16

    def read(self, address, name, budget):
        budget.take_bytes(16)
        if address not in self.nodes:
            raise ReadFailure("unreadable memory")
        return self.nodes[address]

    def read_bytes(self, address, size, budget):
        budget.take_bytes(size)
        return b"x" * size  # Deliberately unterminated string.


class ReaderTests(unittest.TestCase):
    def setUp(self):
        self.memory = Memory()

    def test_null_and_invalid_array_lengths(self):
        self.assertEqual(readers.array(self.memory, 0, "N", 0, 2, Budget()).status, "complete")
        self.assertIn("null", readers.array(self.memory, 0, "N", 1, 2, Budget()).issues[0])
        for count in (-1, 3):
            self.assertIn("invalid", readers.array(self.memory, 16, "N", count, 2, Budget()).issues[0])

    def test_list_cycles_unreadable_and_limits(self):
        result = readers.pointer_list(self.memory, 16, "N", "next", Budget())
        self.assertEqual(len(result.items), 2)
        self.assertEqual(result.status, "complete")
        self.memory.nodes[32]["next"] = 16
        self.assertIn("cycle", readers.pointer_list(self.memory, 16, "N", "next", Budget()).issues[0])
        self.assertIn("unreadable", readers.pointer_list(self.memory, 99, "N", "next", Budget()).issues[0])
        self.assertIn("entry limit", readers.pointer_list(self.memory, 16, "N", "next", Budget(entries=1)).issues[0])
        self.assertIn("byte limit", readers.pointer_list(self.memory, 16, "N", "next", Budget(bytes=1)).issues[0])

    def test_index_cycles_and_out_of_range(self):
        result = readers.index_list(self.memory, 16, "N", 0, 2, "index", -1, Budget())
        self.assertEqual(result.status, "complete")
        self.memory.nodes[32]["index"] = 0
        self.assertIn("cycle", readers.index_list(self.memory, 16, "N", 0, 2, "index", -1, Budget()).issues[0])
        self.memory.nodes[32]["index"] = 2
        self.assertIn("invalid index", readers.index_list(self.memory, 16, "N", 0, 2, "index", -1, Budget()).issues[0])

    def test_string_bytes_and_graph_depth(self):
        result = readers.string(self.memory, 16, Budget(entries=10, bytes=3))
        self.assertEqual(result.items[0]["text"], "xxx")
        self.assertIn("byte limit", result.issues[0])
        result = readers.graph(self.memory, 16, "N", ["next"], Budget(depth=1))
        self.assertEqual(len(result.items), 1)
        self.assertIn("depth limit", result.issues[0])
        self.memory.nodes[32]["next"] = 16
        self.assertIn("cycle or shared", readers.graph(self.memory, 16, "N", ["next"], Budget()).issues[0])

    def test_empty_hash_buckets_are_bounded(self):
        result = readers.buckets(self.memory, 16, "N", "next", [0] * 100, Budget(entries=2))
        self.assertIn("entry limit", result.issues[0])


if __name__ == "__main__":
    unittest.main()
