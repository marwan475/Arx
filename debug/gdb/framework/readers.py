"""Collection algorithms receive a bounded memory reader, never target callbacks."""
from .results import ReadFailure, Result


def array(memory, address, type_name, count, capacity, budget):
    result = Result(source=address, type_name=type_name,
                    count_semantics="declared length; bounded by capacity")
    if count < 0 or capacity < 0 or count > capacity:
        return result.fail("invalid length/capacity")
    if count and not address:
        return result.fail("null root with nonzero length")
    try:
        size = memory.sizeof(type_name)
        for index in range(count):
            budget.take_entry()
            item_address = address + index * size
            value = memory.read(item_address, type_name, budget)
            result.items.append({"address": item_address, "value": value})
    except ReadFailure as error:
        result.fail(error)
    return result


def pointer_list(memory, address, type_name, next_field, budget):
    result = Result(source=address, type_name=type_name,
                    count_semantics="observed link order until null")
    seen = set()
    try:
        while address:
            if address in seen:
                raise ReadFailure("cycle in pointer list")
            seen.add(address)
            budget.take_entry()
            value = memory.read(address, type_name, budget)
            result.items.append({"address": address, "value": value})
            address = int(value[next_field])
    except ReadFailure as error:
        result.fail(error)
    return result


def index_list(memory, base, type_name, head, capacity, next_field, sentinel, budget):
    result = Result(source=base, type_name=type_name,
                    count_semantics="observed index-link order until sentinel")
    seen = set()
    try:
        if capacity < 0:
            raise ReadFailure("invalid capacity")
        size = memory.sizeof(type_name)
        while head != sentinel:
            if head < 0 or head >= capacity:
                raise ReadFailure("invalid index: %d" % head)
            if head in seen:
                raise ReadFailure("cycle in index list")
            if not base:
                raise ReadFailure("null index array")
            seen.add(head)
            budget.take_entry()
            address = base + head * size
            value = memory.read(address, type_name, budget)
            result.items.append({"index": head, "address": address, "value": value})
            head = int(value[next_field])
    except ReadFailure as error:
        result.fail(error)
    return result


def string(memory, address, budget):
    result = Result(source=address, type_name="char",
                    count_semantics="bytes before NUL", units="bytes")
    data = bytearray()
    try:
        if not address:
            raise ReadFailure("null string")
        while True:
            budget.take_entry()
            byte = memory.read_bytes(address + len(data), 1, budget)[0]
            if byte == 0:
                break
            data.append(byte)
    except ReadFailure as error:
        result.fail(error)
    result.items.append({"address": address, "text": data.decode("utf-8", "replace")})
    return result


def buckets(memory, address, node_type, next_field, heads, budget):
    result = Result(source=address, type_name=node_type,
                    count_semantics="bucket order, then link order; aliases retained")
    try:
        for index, head in enumerate(heads):
            # Empty buckets must also consume work budget.
            budget.take_entry()
            chain = pointer_list(memory, int(head), node_type, next_field, budget)
            for item in chain.items:
                result.items.append(dict(item, bucket=index))
            for issue in chain.issues:
                result.fail("bucket %d: %s" % (index, issue))
            if chain.issues:
                break
    except ReadFailure as error:
        result.fail(error)
    return result


def graph(memory, address, type_name, edge_fields, budget):
    result = Result(source=address, type_name=type_name,
                    count_semantics="depth-first nodes; repeated edges reported")
    pending = [(address, 0)]
    seen = set()
    try:
        while pending:
            current, depth = pending.pop()
            if not current:
                continue
            if current in seen:
                result.fail("cycle or shared graph node at %#x" % current)
                continue
            if depth >= budget.depth:
                result.fail("truncated: depth limit")
                continue
            budget.take_entry()
            seen.add(current)
            value = memory.read(current, type_name, budget)
            result.items.append({"address": current, "depth": depth, "value": value})
            for name in reversed(edge_fields):
                pending.append((int(value[name]), depth + 1))
    except ReadFailure as error:
        result.fail(error)
    return result
