"""Adapter for arx.foundation.collections v1. Native contract is in GdbCommands.md."""
from framework import readers
from framework.results import ReadFailure

SEMANTICS = "arx.foundation.collections"


def gather(name, memory, descriptor, budget):
    if int(descriptor["version"]) != 1:
        raise ReadFailure("unsupported foundation view version")
    if int(descriptor["rootSize"]) != memory.sizeof("FoundationCollections"):
        raise ReadFailure("mismatched view root size")
    if int(descriptor["capacity"]) != 4 or int(descriptor["count"]) != 4:
        raise ReadFailure("invalid foundation descriptor bounds")
    root_address = int(descriptor["root"])
    root = memory.read(root_address, "FoundationCollections", budget)
    nodes = root_address  # nodes is the first field, also checked against DWARF.
    node_field = next(field for field in root.type.fields() if field.name == "nodes")
    nodes += node_field.bitpos // 8
    count = int(root["count"])
    if count < 0 or count > 4:
        raise ReadFailure("invalid collection length")
    if name == "array":
        return readers.array(memory, nodes, "FoundationNode", count, 4, budget)
    if name == "list":
        return readers.pointer_list(memory, int(root["head"]), "FoundationNode", "next", budget)
    if name == "indices":
        return readers.index_list(memory, nodes, "FoundationNode", int(root["indexHead"]),
                                  count, "nextIndex", -1, budget)
    if name == "string":
        return readers.string(memory, int(root["label"]), budget)
    if name == "buckets":
        return readers.buckets(memory, root_address, "FoundationNode", "next",
                               [int(root["buckets"][i]) for i in range(2)], budget)
    if name == "graph":
        return readers.graph(memory, int(root["head"]), "FoundationNode", ["next"], budget)
    raise ReadFailure("unknown foundation view")
