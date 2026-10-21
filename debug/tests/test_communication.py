"""Small stopped-memory fixtures for IPC safety, ordering and ownership contracts."""
import unittest
from test_scheduling import Fixture as IdentityFixture, marker
from framework.results import Budget, ReadFailure
from views import communication as view


def text(result):
    return "\n".join(item.get("path", "") + " " + item["text"] for item in result.items)


class Fixture(IdentityFixture):
    def __init__(self, socket=False):
        super().__init__("SocketDebugStats" if socket else "IpcDebugStats", "SocketManager" if socket else "InterProcessCommunicationManager", 128 if socket else 0)
        self.blobs = {}
        self.byte_reads = []
        self.memory.read_bytes = self.read_bytes
        self.memory.records[0x2000, "ProcessDebugStats"].update(vfsRoute=100, fileOperations=[101, 102, 103])
        self.stats.update(processes=0x2000, operations=103, pipeOperations=101, memoryOperations=102, sockets=0)
        self.process.update(descriptorTable=0x70000, addressSpace=0)
        self.memory.records[0x20000, "task_t"]["descriptorTable"] = 0
        self.memory.records[0x70000, "descriptor_table_t"] = dict(count=2, entries=0x71000, references=2, lock=0)
        for index in range(2):
            self.memory.records[0x71000 + index * 256, "file_descriptor_t"] = dict(file=0, operations=100, flags=0)
        self.add_file(0x80000, 101, 0x90000, 0)
        if socket:
            self.root("head", "uint64_t", 0)
            self.root("count", "size_t", 0)

    def add_file(self, pointer, operations, private, fd=None):
        self.memory.records[pointer, "file_t"] = dict(refCount=1, operations=operations, privateData=private,
            inode=private, offset=0, statusFlags=0, ioLock=0, refLock=0)
        if fd is not None:
            self.memory.records[0x71000 + fd * 256, "file_descriptor_t"]["file"] = pointer

    def read_bytes(self, pointer, amount, budget):
        budget.take_bytes(amount)
        self.byte_reads.append((pointer, amount))
        for base, data in self.blobs.items():
            if base <= pointer and pointer + amount <= base + len(data):
                return data[pointer-base:pointer-base+amount]
        raise ReadFailure("unreadable fixture bytes")

    def pipe(self):
        state = 0x90000
        self.memory.arrays[state, "pipe_state_t", "inode"] = state
        self.memory.fields[state, "inode_t", "type"] = 6
        values = dict(reader=0x80000, writer=0, references=1, capacity=4096, head=4094, size=4,
                      lastReadiness=[1, 0], pendingReadiness=[1, 0], debugGroup=marker())
        for field, value in values.items():
            self.memory.fields[state, "pipe_state_t", field] = value
        self.memory.arrays[state, "pipe_state_t", "packet"] = 0xA0000
        self.memory.arrays[state, "pipe_state_t", "data"] = 0xB0000
        metadata = bytearray(1024)
        for physical, flags in ((4094, 1), (4095, 1), (0, 1), (1, 3)):
            metadata[physical // 4] |= flags << ((physical % 4) * 2)
        self.blobs[0xA0000] = bytes(metadata)
        self.blobs[0xB0000] = b"cd" + b"x" * 4092 + b"ab"
        return values

    def endpoint(self):
        self.add_file(0x80000, 103, 0xC0000, 0)
        self.memory.arrays[0xC0000, "socket_endpoint_t", "inode"] = 0xC0000
        unnamed = dict(family=1, path=[0] * 108)
        endpoint = dict(inode=dict(type=5), passCredentials=False, manager=0x1000, file=0x80000, next=0, type=2, peer=0,
                        address=unnamed, peerAddress=unnamed, addressLength=2, peerAddressLength=2,
                        bound=False, binding=False, connected=False, listening=False,
                        backlog=0, pendingCount=0, pendingHead=0, pendingTail=0, pendingNext=0,
                        head=0, tail=0, packetCount=0, queued=0, sendLimit=65536, receiveLimit=65536,
                        readShutdown=False, writeShutdown=False, internalReferences=0,
                        credentials=[1, 2, 3], peerCredentials=[0, -1, -1], lastReadiness=0, pendingReadiness=0)
        self.memory.records[0xC0000, "socket_endpoint_t"] = endpoint
        self.memory.records[self.stats["head"], "uint64_t"] = 0xC0000
        self.memory.records[self.stats["count"], "size_t"] = 1
        return endpoint

    def gather(self, name, selectors=None, limit=1000):
        return (view.sockets if self.root_type == "SocketDebugStats" else view.ipc)(
            name, self.memory, self.descriptor, Budget(limit, 1024 * 1024, 8), selectors)


class CommunicationViews(unittest.TestCase):
    def test_wrapped_packet_metadata_without_payload_read(self):
        fixture = Fixture()
        fixture.pipe()
        result = fixture.gather("pipes", {"--pid": 0, "--fd": 0})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("logical=[0,4)", text(result))
        self.assertIn("closed (null endpoint)", text(result))
        self.assertEqual(fixture.byte_reads, [(0xA0000, 1024)])
        result = fixture.gather("pipes", {"--pid": 0, "--fd": 0, "--payload-bytes": 4})
        self.assertIn("b'ab'", text(result))
        self.assertIn("b'cd'", text(result))

    def test_invalid_operation_and_ring_and_partial_marker(self):
        fixture = Fixture()
        ring = fixture.pipe()
        fixture.memory.fields[0x90000, "pipe_state_t", "capacity"] = 3
        self.assertTrue(any("invalid pipe ring" in issue for issue in fixture.gather("pipes").issues))
        fixture.memory.fields[0x90000, "pipe_state_t", "capacity"] = 4096
        ring["debugGroup"]["activeWriters"] = 1
        self.assertEqual(fixture.gather("pipes").status, "partial")
        fixture.memory.records[0x71000, "file_descriptor_t"]["operations"] = 999
        self.assertTrue(any("opaque handle" in issue for issue in fixture.gather("pipes", {"--pid": 0}).issues))
        self.assertEqual(fixture.gather("pipes", limit=1).status, "partial")

    def test_abstract_embedded_nul_and_invalid_length(self):
        value = dict(family=1, path=[0, 97, 0, 98] + [0] * 104)
        self.assertEqual(view.address_text(value, 6), "abstract b'a\\x00b'")
        self.assertEqual(view.address_text(value, 2), "unnamed")
        with self.assertRaises(ReadFailure):
            view.address_text(value, 111)

    def test_queued_right_and_payload_offset(self):
        fixture = Fixture(True)
        endpoint = fixture.endpoint()
        endpoint.update(head=0xD0000, tail=0xD0000, packetCount=1, queued=3)
        packet = dict(next=0, size=5, offset=2, control=0xE0000, source=endpoint["address"], sourceLength=0)
        fixture.memory.records[0xD0000, "socket_packet_t"] = packet
        fixture.memory.arrays[0xD0000, "socket_packet_t", "data"] = 0xF0000
        fixture.blobs[0xF0000] = b"abcde"
        fixture.add_file(0x81000, 102, 0x91000)
        control = dict(next=0, count=1, files=[0x81000] + [0] * 252,
                       credentials=dict(pid=9, uid=10, gid=11), explicitCredentials=True, senderUid=10)
        fixture.memory.records[0xE0000, "socket_ancillary_t"] = control
        result = fixture.gather("messages", {"--value": 0xC0000})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("owning queued right", text(result))
        self.assertIn("pid=9 uid=10 gid=11", text(result))
        self.assertFalse(fixture.byte_reads)
        result = fixture.gather("messages", {"--value": 0xC0000, "--payload-bytes": 2})
        self.assertIn("b'cd'", text(result))
        self.assertEqual(fixture.byte_reads, [(0xF0002, 2)])
        control["count"] = 254
        self.assertTrue(any("invalid ancillary rights count" in issue for issue in fixture.gather("messages", {"--value": 0xC0000}).issues))

    def test_invalid_endpoint_cycle_and_packet_offset(self):
        fixture = Fixture(True)
        endpoint = fixture.endpoint()
        endpoint["next"] = 0xC0000
        self.assertTrue(any("cycle" in issue for issue in fixture.gather("endpoints").issues))
        endpoint["next"] = 0
        self.assertTrue(any("absent" in issue for issue in fixture.gather("messages", {"--value": 0x1234}).issues))
        endpoint.update(head=0xD0000, tail=0xD0000, packetCount=1, queued=1)
        fixture.memory.records[0xD0000, "socket_packet_t"] = dict(next=0, size=1, offset=2, control=0)
        self.assertTrue(any("invalid packet size/offset" in issue for issue in fixture.gather("endpoints").issues))

    def test_memory_seals_page_and_backing_identity(self):
        fixture = Fixture()
        fixture.add_file(0x80000, 102, 0x91000, 0)
        fixture.memory.arrays[0x91000, "memory_file_t", "inode"] = 0x91000
        inode = dict(type=0, size=4096, backing=0x92000, ioLock=0)
        fixture.memory.fields[0x91000, "inode_t", "type"] = 0
        fixture.memory.records[0x91000, "memory_file_t"] = dict(inode=inode, seals=12, pages=[0x93000] + [0] * 255, debugGroup=marker())
        fixture.memory.records[0x92000, "virt_backing_object_t"] = dict(size=4096, references=2, mappings=1,
            writable_mappings=0, file=True, volatile_storage=True, operations=0, context=0, lock=0, pages=0)
        fixture.memory.records[0x93000, "virt_shared_page_t"] = dict(offset=0, pointer=0x94000, references=2, dirty=False)
        fixture.process["addressSpace"] = 0x95000
        fixture.memory.records[0x95000, "virt_addr_space_t"] = dict(operation_lock=0, kernel_used_regions=0, user_used_regions=0x96000)
        fixture.memory.records[0x96000, "virt_region_t"] = dict(next=0, object=0x92000, start=0x1000, end=0x2000, backing_offset=0, shared_mapping=True, shared=0x93000)
        result = fixture.gather("memory-file", {"--pid": 0, "--fd": 0})
        self.assertEqual(result.status, "complete", result.issues)
        self.assertIn("GROW,WRITE", text(result))
        self.assertIn("sharedPage=0x93000", text(result))
        self.assertIn("mapping pid=0", text(result))
        self.assertFalse(fixture.byte_reads)


if __name__ == "__main__":
    unittest.main()
