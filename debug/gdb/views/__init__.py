"""Semantic adapter table: (semantic ID, version) -> (root type, gather function).

Adding fields to stats never needs an adapter. Add an entry here only when a rich
collection needs interpretation beyond generic DWARF rendering.
"""
from . import foundation

ADAPTERS = {
    (foundation.SEMANTICS, 1): ("FoundationCollections", foundation.gather),
}
