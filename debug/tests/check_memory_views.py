#!/usr/bin/env python3
"""Host-only corruption/budget fixtures; no target allocation or writes."""
import copy
from pathlib import Path
import shlex
import sys
import types
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gdb"))
sys.modules.setdefault("gdb", types.SimpleNamespace(Command=object, string_to_argv=shlex.split))
from framework.results import Budget
from framework import commands
from views import memory as views


class Memory:
    def __init__(self):
        self.fields = {}
        self.values = {}
        self.raw = {}

    def sizeof(self, name):
        return {"page_t": 40, "slab_t": 72, "numa_node_t": 16384,
                "cpu_info_t": 512, "kernel_heap_t": 4096}.get(name, 128)

    def read(self, address, name, budget):
        budget.take_bytes(self.sizeof(name))
        if (address, name) not in self.values:
            raise views.ReadFailure("unreadable fixture address")
        return copy.deepcopy(self.values[address, name])

    def field(self, address, name, field, budget):
        budget.take_bytes(8)
        return copy.deepcopy(self.fields[address, name, field])

    def field_address(self, address, name, field):
        offsets = {"numa_nodes": 8192, "heap": 8192, "zone": 0, "dma_zone": 4096}
        return address + offsets[field], None

    def read_bytes(self, address, size, budget):
        budget.take_bytes(size)
        try:
            return bytes(self.raw[address + offset] for offset in range(size))
        except KeyError as error:
            raise views.ReadFailure("unreadable fixture bitmap") from error


def operations():
    return dict.fromkeys(("attempts", "successes", "failures", "frees", "requestedBytes",
                          "roundedBytes", "liveBlocks", "liveRoundedBytes"), 0)


def descriptor(kind):
    return {"root": 100, "rootSize": 128, "version": 1, "count": 0,
            "capacity": 0 if kind == "dma" else 8}


def fixture(kind):
    memory = Memory()
    stats = {"source": 4096, "capacity": 8, "ready": 1, "metadataCapacity": 64,
             "metadataReservedBytes": 4096, "allocationCalls": 0, "allocationFailures": 0, "freeCalls": 0}
    memory.values[100, "arx_%s_debug_stats_t" % kind] = stats
    memory.fields[4096, "platform_t", "numa_node_count"] = 1
    memory.fields[4096, "platform_t", "cpu_count"] = 4
    memory.fields[4096, "platform_t", "cpus"] = 65536
    for slot in range(4):
        memory.fields[65536 + slot * 512, "cpu_info_t", "id"] = slot + 10
        memory.fields[65536 + slot * 512, "cpu_info_t", "numa_node"] = 12288
    memory.fields[12288, "numa_node_t", "proximity_domain"] = 7
    zone = {"region_count": 1, "regions": [{"start_pfn": 16, "end_pfn": 20, "page_count": 4}],
            "debugStats": {"group": {"activeWriters": 0}, "operations": operations(), "freeBlocks": [0, 0, 1] + [0] * 8},
            "total_pages": 4, "free_pages": 4, "used_pages": 0,
            "buddy_metadata": 131072, "buddy_free_lists": [{"head": 0} for _ in range(11)],
            "lock": 0, "min_pfn": 0, "max_pfn": 64, "hhdm_present": 1, "hhdm_offset": 0, "total_memory": 16384}
    zone["buddy_free_lists"][2]["head"] = 131072 + 16 * 40
    memory.fields[12288, "numa_node_t", "zone"] = zone
    dma = copy.deepcopy(zone)
    dma["regions"] = [{"start_pfn": 24, "end_pfn": 28, "page_count": 4}]
    dma["buddy_free_lists"][2]["head"] = 131072 + 24 * 40
    memory.fields[12288, "numa_node_t", "dma_zone"] = dma
    for pfn in (16, 24):
        memory.values[131072 + pfn * 40, "page_t"] = {"pfn": pfn, "flags": 1, "order": 2, "prev": 0, "next": 0}
    caches = []
    for size in views.CLASSES:
        debug = {"operations": operations(), "slabsInitialized": 0, "payloadBackingBytes": 0,
                 "bitmapBackingBytes": 0, "slabInitFailures": 0, "retainedInitFailureBytes": 0}
        caches.append({"object_size": size, "debugStats": debug, "partial_slabs": 0,
                       "full_slabs": 0, "empty_slabs": 0,
                       "slab_metadata_pool": {"chunks": 0, "free_list": 0, "element_size": 72, "elements_per_chunk": 4}})
    caches[2]["partial_slabs"] = 300000
    caches[2]["slab_metadata_pool"]["chunks"] = 700000
    caches[2]["debugStats"]["operations"]["liveBlocks"] = 1
    memory.values[700000, "metadata_chunk_t"] = {"prev": 0, "next": 0, "data": 300000, "size": 288}
    memory.values[20480, "kernel_heap_t"] = {"lock": 0, "debugGroup": {"activeWriters": 0}, "caches": caches}
    memory.values[300000, "slab_t"] = {"prev": 0, "next": 0, "object_size": 64,
        "total_objects": 2048, "free_objects": 2047, "allocated": 1, "objects_array": 400000, "object_bitmap": 600000}
    memory.raw = {600000 + offset: int(offset == 0) for offset in range(256)}
    return memory


def dma_fixture():
    memory = Memory()
    stats = {"source": 4096, "backendKind": 2, "group": {"activeWriters": 0},
             "allocationCalls": 1, "freeCalls": 0, "allocationOutcomes": [0, 0, 0, 0, 1, 0, 0],
             "freeOutcomes": [0] * 7, "rollbackRetained": 1, "cleanupCompletions": 0,
             "successfulRecords": 0, "cleanupRecords": 1, "retainedRequestedBytes": 5000,
             "retainedMappedBytes": 5000, "retainedReservedBytes": 8192}
    memory.values[100, "DmaDebugStats"] = stats
    memory.fields[4096, "DmaManager", "AllocationCount"] = 1
    memory.fields[4096, "DmaManager", "Allocations"] = 800000
    memory.fields[4096, "DmaManager", "Operations"] = dict.fromkeys(
        ("context", "query", "map", "unmap", "writeBarrier", "readBarrier"), 1234)
    memory.values[800000, "DmaManager::AllocationRecord"] = {
        "allocation": {"id": 17, "cpuAddress": 0, "dmaAddress": {"value": 0}, "size": 0},
        "debug": {"phase": 2, "cleanupOnly": 1, "backingHeld": 1, "mappingHeld": 1,
                  "effectiveAlignment": 64, "effectiveBoundary": 8192,
                  "requestedAlignment": 1, "requestedBoundary": 0,
                  "physicalMask": (1 << 32) - 1, "lastCleanupStatus": 4},
        "device": {"addressMask": (1 << 32) - 1, "minimumAlignment": 64,
                   "boundary": 8192, "pciDevice": 0},
        "mapping": {"address": {"value": (1 << 64) - 1}, "cookie": 1234},
        "mappedSize": 5000, "reservedSize": 8192, "backing": 900000,
        "physical": 4096, "releasing": 0, "next": 0}
    return memory


class Views(unittest.TestCase):
    def pmm(self, name="nodes", selectors=None, budget=None, memory=None):
        return views.pmm(name, memory or fixture("pmm"), descriptor("pmm"), budget or Budget(), selectors)

    def heap(self, name="address", selectors=None, budget=None, memory=None):
        return views.heap(name, memory or fixture("heap"), descriptor("heap"), budget or Budget(), selectors or {"--value": 400001})

    def test_unique_nodes_and_dma_ranges(self):
        result = self.pmm()
        self.assertEqual(result.status, "complete")
        self.assertEqual(sum(item["path"] == "Node 0" for item in result.items), 1)
        self.assertIn("13 (slot 3)", str(result.items))
        self.assertIn("/ dma", str(result.items))

    def test_controlled_second_node_and_cross_node_ownership(self):
        memory = fixture("pmm")
        memory.fields[4096, "platform_t", "numa_node_count"] = 2
        memory.fields[12288 + 16384, "numa_node_t", "proximity_domain"] = 9
        for field, start in (("zone", 32), ("dma_zone", 40)):
            zone = copy.deepcopy(memory.fields[12288, "numa_node_t", field])
            zone["regions"] = [{"start_pfn": start, "end_pfn": start + 4, "page_count": 4}]
            memory.fields[12288 + 16384, "numa_node_t", field] = zone
        memory.fields[65536 + 3 * 512, "cpu_info_t", "numa_node"] = 12288 + 16384
        result = self.pmm(memory=memory)
        self.assertEqual(result.status, "complete")
        self.assertIn("domain=9", str(result.items))
        self.assertIn("Node 1", str(result.items))
        memory.fields[12288 + 16384, "numa_node_t", "zone"]["regions"] = [{"start_pfn": 16, "end_pfn": 20, "page_count": 4}]
        self.assertIn("overlapping physical ownership", str(self.pmm(memory=memory).issues))

    def test_cpu_and_node_selectors(self):
        self.assertEqual(self.pmm(selectors={"--cpu": 12}).status, "complete")
        self.assertEqual(self.pmm(selectors={"--node": 1}).status, "partial")
        self.assertEqual(self.pmm(selectors={"--slot": 7}).status, "partial")

    def test_pfn_free_head_and_stale_interior(self):
        self.assertIn("validated free block head", str(self.pmm("pfn", {"--value": 16}).items))
        memory = fixture("pmm")
        memory.values[131072 + 17 * 40, "page_t"] = {"pfn": 17, "flags": 2, "order": 0}
        result = self.pmm("pfn", {"--value": 17}, memory=memory)
        self.assertIn("ownership unknown", str(result.items))
        self.assertNotIn("validated free block head", str(result.items))

    def test_invalid_pfn_and_metadata_link(self):
        self.assertEqual(self.pmm("pfn", {"--value": 64}).status, "partial")
        memory = fixture("pmm")
        memory.fields[12288, "numa_node_t", "zone"]["buddy_free_lists"][2]["head"] = 1
        self.assertIn("outside the metadata", str(self.pmm("blocks", {"--order": 2}, memory=memory).issues))

    def test_cycle_and_count_disagreement(self):
        memory = fixture("pmm")
        memory.values[131072 + 16 * 40, "page_t"]["next"] = 131072 + 16 * 40
        self.assertIn("cycle", str(self.pmm("blocks", {"--order": 2}, memory=memory).issues))
        memory = fixture("pmm")
        memory.fields[12288, "numa_node_t", "zone"]["debugStats"]["freeBlocks"][2] = 2
        self.assertEqual(self.pmm(memory=memory).status, "partial")

    def test_early_and_interrupted_state(self):
        memory = fixture("pmm")
        memory.values[100, "arx_pmm_debug_stats_t"]["ready"] = 0
        self.assertIn("initialization incomplete", str(self.pmm(memory=memory).issues))
        memory = fixture("pmm")
        memory.fields[12288, "numa_node_t", "zone"]["debugStats"]["group"]["activeWriters"] = 2
        self.assertIn("interrupted", str(self.pmm(memory=memory).issues))

    def test_limits_stop_scans(self):
        for budget in (Budget(entries=2), Budget(bytes=128)):
            self.assertEqual(self.pmm("blocks", budget=budget).status, "partial")
        self.assertEqual(self.heap(budget=Budget(entries=1)).status, "partial")

    def test_heap_interior_and_bitmap(self):
        result = self.heap(selectors={"--value": 400001, "--check-bitmap": True})
        self.assertEqual(result.status, "complete")
        self.assertIn("interior address; allocated=True", str(result.items))
        self.assertIn("bitmap allocated=1", str(result.items))
        memory = fixture("heap")
        memory.raw[600000] = 0
        self.assertIn("bitmap and free-object", str(self.heap(selectors={"--value": 400000, "--check-bitmap": True}, memory=memory).issues))

    def test_heap_bad_links_and_counts(self):
        memory = fixture("heap")
        memory.values[300000, "slab_t"]["free_objects"] = 2049
        self.assertEqual(self.heap(memory=memory).status, "partial")
        memory = fixture("heap")
        memory.values[300000, "slab_t"]["next"] = 300000
        self.assertIn("cycle", str(self.heap(memory=memory).issues))

    def test_dma_cleanup_retention_and_phase(self):
        memory = dma_fixture()
        result = views.dma("allocations", memory, descriptor("dma"), Budget())
        self.assertEqual(result.status, "complete")
        self.assertIn("cleanup-only failed allocation", str(result.items))
        self.assertIn("8192 reserved backing bytes", str(result.items))
        self.assertIn("satisfies constraints=False", str(result.items))
        memory.values[100, "DmaDebugStats"]["group"]["activeWriters"] = 2
        memory.values[800000, "DmaManager::AllocationRecord"]["debug"]["phase"] = 3
        result = views.dma("allocations", memory, descriptor("dma"), Budget())
        self.assertEqual(result.status, "partial")
        self.assertIn("unmapping", str(result.items))

    def test_dma_cycle_and_totals(self):
        memory = dma_fixture()
        memory.values[800000, "DmaManager::AllocationRecord"]["next"] = 800000
        self.assertIn("cycle", str(views.dma("allocations", memory, descriptor("dma"), Budget()).issues))
        memory = dma_fixture()
        memory.values[100, "DmaDebugStats"]["retainedReservedBytes"] = 4096
        self.assertIn("totals disagree", str(views.dma("allocations", memory, descriptor("dma"), Budget()).issues))
        self.assertEqual(views.dma("allocations", dma_fixture(), descriptor("dma"), Budget(entries=1)).status, "partial")

    def test_range_constraint_edge_cases(self):
        self.assertTrue(views.range_valid(0, 1, (1 << 64) - 1, 1, 0))
        self.assertFalse(views.range_valid((1 << 64) - 8, 16, (1 << 64) - 1, 1, 0))
        self.assertFalse(views.range_valid(4095, 2, (1 << 64) - 1, 1, 4096))
        self.assertFalse(views.range_valid(4096, 1, 4095, 1, 0))

    def test_parser_preserves_zero_and_boolean_options(self):
        words, _, options = commands.parse("pmm blocks --node 0 --zone dma --order 0 --limit 2")
        self.assertEqual(words, ["pmm", "blocks"])
        self.assertEqual(options, {"--node": 0, "--zone": "dma", "--order": 0})
        self.assertTrue(commands.parse("heap slabs --check-bitmap")[2]["--check-bitmap"])

    def test_parser_rejects_ambiguity_and_expressions(self):
        for text in ("pmm nodes --node 0 --cpu 0", "pmm blocks --order 11", "pmm zones --zone other",
                     "heap slabs --check-bitmap --check-bitmap", "pmm nodes --node foo()"):
            with self.assertRaises(views.ReadFailure):
                commands.parse(text)
        with self.assertRaises(views.ReadFailure):
            commands.gather("heap address call()")


if __name__ == "__main__":
    unittest.main()
