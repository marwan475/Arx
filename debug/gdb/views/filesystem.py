"""Read-only archive and namespace interpretation; FileSystemDebugStats.hpp v1."""
from framework.results import ReadFailure, Result
from .memory import descriptor_root, group, number
from .platform import row

ARCHIVE_SEMANTICS = "arx.filesystem.archive"
VFS_SEMANTICS = "arx.filesystem.namespace"
KINDS = {0: "file", 1: "directory", 2: "symlink", 3: "character device",
         4: "block device", 5: "socket", 6: "FIFO"}
ADDRESS_LIMIT = 1 << 64


class Reader:
    def __init__(self, memory, budget, result, scan=4096):
        self.memory = memory
        self.budget = budget
        self.result = result
        self.remaining = scan
        self.archives = {}

    def work(self):
        if self.remaining <= 0:
            raise ReadFailure("truncated: filesystem scan limit")
        self.remaining -= 1

    def read(self, address, type_name):
        size = self.memory.sizeof(type_name)
        if address <= 0 or address + size > ADDRESS_LIMIT:
            raise ReadFailure("invalid or overflowing " + type_name + " address")
        return self.memory.read(address, type_name, self.budget)

    def pointer(self, address):
        return int(self.read(address, "uint64_t"))

    def string(self, address, maximum=512):
        if not address:
            raise ReadFailure("unavailable: null filesystem string")
        data = bytearray()
        for index in range(maximum):
            byte = self.memory.read_bytes(address + index, 1, self.budget)[0]
            if byte == 0:
                return data.decode("utf-8", "strict")
            data.append(byte)
        raise ReadFailure("unterminated or excessive filesystem string")

    def linked(self, address, type_name, next_field="next"):
        seen = set()
        while address:
            self.work()
            if address in seen or address % 8:
                raise ReadFailure("cycle or misaligned link in " + type_name)
            seen.add(address)
            value = self.read(address, type_name)
            yield address, value
            address = number(value, next_field)

    def archive(self, owner):
        if owner in self.archives:
            return self.archives[owner]
        stats = self.memory.field(owner, "InitRamFileSystemManager", "DebugStats", self.budget)
        if number(stats, "source") != owner or number(stats, "capacity") != 256:
            raise ReadFailure("invalid archive source/capacity")
        group(self.result, stats["group"], "Archive update")
        if number(stats, "ready") != 1:
            raise ReadFailure("archive unavailable: parsing did not complete")
        count = int(self.read(number(stats, "count"), "size_t"))
        if not 0 <= count <= 256:
            raise ReadFailure("archive count exceeds capacity")
        base = number(stats, "records")
        if count and not base:
            raise ReadFailure("null archive array with nonzero count")
        entries = []
        for index in range(count):
            self.work()
            address = base + index * self.memory.sizeof("initramfs_archive_t")
            value = self.read(address, "initramfs_archive_t")
            path = self.string(number(value, "path"))
            entries.append((path, address, value))
        self.archives[owner] = entries
        return entries


def path_parts(path):
    parts = path.split("/") if path else []
    if any(part in ("", ".", "..") or len(part.encode()) >= 256 for part in parts):
        raise ReadFailure("ambiguous or unsupported stored path: " + repr(path))
    if len(path.encode()) >= 512:
        raise ReadFailure("excessive filesystem path")
    return parts


def selected_path(selectors):
    path = selectors.get("--path", "/")
    if not path.startswith("/"):
        raise ReadFailure("--path requires an absolute path")
    path = path.rstrip("/") or "/"
    path_parts(path[1:])
    return path


def archive_tree(reader, owner):
    tree = {"": {"kind": 1, "size": 0, "mode": 0o755, "uid": 0, "gid": 0,
                  "source": "inferred directory", "data": 0, "record": 0}}
    explicit = set()
    for path, address, value in reader.archive(owner):
        mode = number(value, "mode")
        if path == "." and mode & 0o170000 == 0o040000:
            row(reader.result, reader.budget, "archive root marker", "stored name='.' record=%#x raw mode=%#o UID=%d GID=%d; not a backend child or root-attribute override" % (
                address, mode, number(value, "uid"), number(value, "gid")))
            continue
        try:
            parts = path_parts(path)
        except ReadFailure as error:
            reader.result.fail(error)
            row(reader.result, reader.budget, "uninterpreted archive path", repr(path))
            continue
        if not parts:
            reader.result.fail("empty archive path")
            continue
        for depth in range(1, len(parts)):
            reader.work()
            parent = "/".join(parts[:depth])
            tree.setdefault(parent, {"kind": 1, "size": 0, "mode": 0o755, "uid": 0,
                                     "gid": 0, "source": "inferred directory", "data": 0, "record": 0})
        if path in explicit:
            reader.result.fail("duplicate archive name; first exact entry wins: " + repr(path))
            continue
        explicit.add(path)
        mode = number(value, "mode")
        tree[path] = {"kind": 1 if mode & 0o170000 == 0o040000 else 0,
                      "size": number(value, "size"), "mode": mode & 0o7777,
                      "rawMode": mode, "uid": number(value, "uid"), "gid": number(value, "gid"),
                      "source": "archive", "data": number(value, "data"), "record": address}
    return tree


def metadata_text(value):
    return "%s size=%d bytes mode=%04o UID=%d GID=%d source=%s" % (
        KINDS.get(value["kind"], "unknown"), value["size"], value["mode"],
        value["uid"], value["gid"], value["source"])


def archive_view(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="initramfs",
                    count_semantics="current parsed archive; hierarchy inferred from stored names", units="sizes are bytes; modes are octal")
    try:
        stats = descriptor_root(memory, descriptor, "InitRamDebugStats", 256, budget)
        owner = selectors.get("--initramfs", number(stats, "source"))
        reader = Reader(memory, budget, result, selectors.get("--scan", 4096))
        if name == "entry":
            if "--path" not in selectors:
                raise ReadFailure("initramfs entry requires --path exact-stored-name")
            path = selectors["--path"]
            if path.startswith("/"):
                path = path[1:]
            matches = [(address, value) for stored, address, value in reader.archive(owner) if stored == path]
            if not matches:
                raise ReadFailure("archive entry not found: " + repr(path))
            address, value = matches[0]
            row(result, budget, path, "record=%#x size=%d bytes data=%#x raw mode=%#o UID=%d GID=%d; payload not read" % (
                address, number(value, "size"), number(value, "data"), number(value, "mode"), number(value, "uid"), number(value, "gid")))
        elif name == "contents":
            tree = archive_tree(reader, owner)
            selected = selected_path(selectors)[1:]
            if selected not in tree:
                raise ReadFailure("archive path not found")
            base_depth = len(selected.split("/")) if selected else 0
            for path in sorted(tree):
                if selected and path != selected and not path.startswith(selected + "/"):
                    continue
                depth = (len(path.split("/")) if path else 0) - base_depth
                if depth > budget.depth:
                    result.fail("truncated: archive hierarchy depth limit")
                    continue
                value = tree[path]
                row(result, budget, "  " * depth + "/" + path, metadata_text(value) +
                    "; record=%#x data=%#x raw mode=%#o" % (value["record"], value["data"], value.get("rawMode", 0)))
        else:
            raise ReadFailure("unsupported archive view")
    except (ReadFailure, ValueError, UnicodeError, OverflowError) as error:
        result.fail(error)
    return result


class Namespace(Reader):
    def __init__(self, memory, budget, result, stats, scan):
        super().__init__(memory, budget, result, scan)
        self.stats = stats
        self.mounts = {}
        self.dentries = {}
        self.inodes = {}
        self.children = {}
        self.retired = []
        self.trees = {}
        self.root = number(self.read(number(stats, "namespaceRoot"), "vfs_namespace_t"), "rootMount")
        group(result, stats["group"], "VFS update")
        for field in ("mountLock", "cacheLock", "overlayLock", "mutationLock"):
            # spinlock_t is a byte; do not accidentally read adjacent members.
            if int(self.read(number(stats, field), "spinlock_t")):
                result.fail(field + " held; namespace may be transitional")
        for address, value in self.linked(self.pointer(number(stats, "mounts")), "mount_t"):
            self.mounts[address] = value
        if self.root and self.root not in self.mounts:
            raise ReadFailure("namespace root is missing from mount list")

    def dentry(self, pointer):
        if pointer not in self.dentries:
            self.work()
            self.dentries[pointer] = self.read(pointer, "dentry_t")
        return self.dentries[pointer]

    def inode(self, pointer):
        if pointer not in self.inodes:
            self.work()
            self.inodes[pointer] = self.read(pointer, "inode_t")
        return self.inodes[pointer]

    def current(self, pointer):
        seen = set()
        while pointer:
            if pointer in seen or len(seen) > self.budget.depth:
                raise ReadFailure("cyclic or excessive rename forwarding")
            seen.add(pointer)
            value = self.dentry(pointer)
            if bool(value["unlinked"]):
                return 0
            moved = number(value, "renamedTo")
            if not moved:
                return pointer
            pointer = moved
        return 0

    def cache(self):
        cache_address = self.pointer(number(self.stats, "cacheRoot"))
        found = []
        if cache_address:
            cache = self.read(cache_address, "khashp_t")
            bits, count = number(cache, "bits"), number(cache, "count")
            base, bitmap = number(cache, "b"), number(cache, "used")
            key_size = self.memory.sizeof("dentry_key_t")
            if number(cache, "key_len") != key_size or number(cache, "val_len") != 8 or not 0 <= bits <= 20:
                raise ReadFailure("invalid VFS hash bucket dimensions")
            capacity = (1 << bits) if base else 0
            if count > capacity or (capacity and not bitmap):
                raise ReadFailure("invalid VFS hash count/storage")
            row(self.result, self.budget, "cache inventory", "cached only: %d occupied of %d buckets; empty buckets consume scan budget" % (count, capacity))
            occupied = 0
            word = 0
            for index in range(capacity):
                self.work()
                if index % 32 == 0:
                    word = int(self.read(bitmap + index // 32 * 4, "uint32_t"))
                if not word & (1 << (index % 32)):
                    continue
                occupied += 1
                location = base + index * (key_size + 8)
                key = self.read(location, "dentry_key_t")
                pointer = self.pointer(location + key_size)
                value = self.dentry(pointer)
                if number(key, "parent") != number(value, "parent") or self.string(number(key, "name"), 256) != self.string(number(value, "name"), 256):
                    self.result.fail("cache key differs from current dentry")
                found.append((pointer, "cache"))
            if occupied != count:
                self.result.fail("hash occupancy differs from stored count")
        overlays = []
        for _, entry in self.linked(self.pointer(number(self.stats, "overlays")), "vfs_overlay_dentry_entry_t"):
            overlays.append((number(entry, "dentry"), "overlay"))
        self.retired = list(self.linked(self.pointer(number(self.stats, "retired")), "dentry_t", "retiredNext"))
        # First overlay match wins, then first occupied cache bucket, as Lookup.
        for pointer, source in overlays + found:
            value = self.dentry(pointer)
            name = self.string(number(value, "name"), 256)
            if bool(value["unlinked"]) or number(value, "renamedTo"):
                continue
            parent = number(value, "parent")
            if parent:
                parent = self.current(parent)
                if parent:
                    self.children.setdefault(parent, {}).setdefault(name, (pointer, source))
        return overlays + found

    def relative(self, pointer, root):
        parts = []
        seen = set()
        pointer = self.current(pointer)
        root = self.current(root)
        while pointer != root:
            if not pointer or pointer in seen or len(parts) >= self.budget.depth:
                raise ReadFailure("missing/cyclic/excessive dentry ancestry")
            seen.add(pointer)
            value = self.dentry(pointer)
            name = self.string(number(value, "name"), 256)
            path_parts(name)
            parts.append(name)
            pointer = self.current(number(value, "parent"))
        return "/".join(reversed(parts))

    def cached_path(self, pointer):
        parts = []
        seen = set()
        while pointer:
            if pointer in seen or len(seen) > self.budget.depth:
                raise ReadFailure("cyclic/excessive cached ancestry")
            seen.add(pointer)
            entry = self.dentry(pointer)
            name = self.string(number(entry, "name"), 256)
            if name != "/":
                path_parts(name)
                parts.append(name)
            pointer = self.current(number(entry, "parent"))
        return "/" + "/".join(reversed(parts))

    def mount_path(self, address, seen=None):
        seen = set() if seen is None else seen
        if address in seen or len(seen) > self.budget.depth or address not in self.mounts:
            raise ReadFailure("missing/cyclic/excessive mount ancestry")
        if address == self.root:
            return "/"
        seen.add(address)
        value = self.mounts[address]
        parent = number(value, "parentMount")
        if parent not in self.mounts:
            raise ReadFailure("detached mount or missing parent")
        suffix = self.relative(number(value, "mountPoint"), number(self.mounts[parent], "root"))
        base = self.mount_path(parent, seen)
        return base.rstrip("/") + "/" + suffix

    def cross(self, mount, dentry):
        seen = set()
        while dentry:
            identity = (mount, dentry)
            if identity in seen or len(seen) > self.budget.depth:
                raise ReadFailure("cyclic/excessive mount crossing")
            seen.add(identity)
            child = next((address for address, value in self.mounts.items()
                          if number(value, "parentMount") == mount and number(value, "mountPoint") == dentry), None)
            if child is None:
                break
            mount = child
            dentry = number(self.mounts[mount], "root")
        return mount, dentry

    def node(self, mount, dentry, fallback=None):
        mount, dentry = self.cross(mount, dentry)
        node = dict(fallback or {})
        node.update(mount=mount, dentry=dentry)
        if dentry:
            value = self.dentry(self.current(dentry))
            inode_address = number(value, "inode")
            inode = self.inode(inode_address)
            node.update(kind=number(inode, "type"), size=number(inode, "size"), mode=number(inode, "mode"),
                        uid=number(inode, "uid"), gid=number(inode, "gid"), inode=inode_address,
                        generation=number(inode, "namespaceGeneration"), metadataValid=bool(inode["metadataValid"]), source="cached VFS metadata")
            supported = number(inode, "inodeOps") == number(self.stats, "resourceInodeOperations") or number(inode, "fileOps") == number(self.stats, "resourceFileOperations")
            node.pop("backend", None)
            if supported:
                private = self.read(number(inode, "privateData"), "vfs_resource_inode_private_t")
                if not number(self.stats, "resourceCaps") or number(private, "caps") != number(self.stats, "resourceCaps"):
                    self.result.fail("unverified Resource filesystem capability identity")
                else:
                    backend = self.read(number(private, "backendNode"), "resource_node_t")
                    filesystem = self.read(number(backend, "fs"), "resource_fs_t")
                    owner = number(filesystem, "initRamManager")
                    path = self.string(number(backend, "path"))
                    path_parts(path)
                    if owner not in self.trees:
                        self.trees[owner] = archive_tree(self, owner)
                    node["backend"] = (owner, path)
                    node["source"] = "cpio + cached VFS metadata"
                    if path in self.trees[owner]:
                        node["backendSize"] = self.trees[owner][path]["size"]
            if number(inode, "fileOps") == number(self.stats, "symlinkFileOperations"):
                private = self.read(number(inode, "privateData"), "vfs_symlink_private_t")
                node["target"] = self.string(number(private, "target"))
        return node

    def candidates(self, node):
        if node["kind"] != 1:
            return {}
        result = {}
        if "backend" in node:
            owner, parent = node["backend"]
            prefix = parent + "/" if parent else ""
            for path, metadata in self.trees[owner].items():
                self.work()
                if path.startswith(prefix) and path != parent:
                    suffix = path[len(prefix):]
                    if "/" not in suffix:
                        child = dict(metadata)
                        child["backend"] = (owner, path)
                        result[suffix] = (0, child)
        else:
            self.result.fail("backend enumeration unavailable; known paths only at mount=%#x dentry=%#x" % (node["mount"], node["dentry"]))
        for name, (pointer, source) in self.children.get(node["dentry"], {}).items():
            path_parts(name)
            result[name] = (pointer, None)
        # A registered mount point is also authoritative even if not cached.
        for value in self.mounts.values():
            if number(value, "parentMount") != node["mount"]:
                continue
            point = number(value, "mountPoint")
            if point:
                entry = self.dentry(point)
                if self.current(number(entry, "parent")) == node["dentry"]:
                    result[self.string(number(entry, "name"), 256)] = (point, None)
        return result

    def contents(self, selected):
        if not self.root:
            raise ReadFailure("unavailable: namespace root not mounted")
        node = self.node(self.root, number(self.mounts[self.root], "root"))
        for component in selected[1:].split("/") if selected != "/" else []:
            choices = self.candidates(node)
            if component not in choices:
                raise ReadFailure("selected path not found in supported/known contents")
            pointer, fallback = choices[component]
            node = self.node(node["mount"], pointer, fallback)
        def visit(current, path, depth, active):
            self.work()
            identity = (current["mount"], current["dentry"], current.get("backend"))
            if identity in active:
                raise ReadFailure("cycle in visible namespace")
            text = metadata_text(current) + "; mount=%#x dentry=%#x inode=%#x generation=%d" % (
                current["mount"], current["dentry"], current.get("inode", 0), current.get("generation", 0))
            if "metadataValid" in current:
                text += "; cached metadata valid=%s" % current["metadataValid"]
            if "backendSize" in current and current["backendSize"] != current["size"]:
                text += "; current backend size=%d bytes (cache differs)" % current["backendSize"]
            if "target" in current:
                text += "; target=" + repr(current["target"]) + " (not followed)"
            row(self.result, self.budget, "  " * depth + path, text)
            choices = self.candidates(current)
            if choices and depth >= self.budget.depth:
                self.result.fail("truncated: namespace depth limit at " + path)
                return
            for name in sorted(choices):
                pointer, fallback = choices[name]
                child = self.node(current["mount"], pointer, fallback)
                visit(child, path.rstrip("/") + "/" + name, depth + 1, active | {identity})
        visit(node, selected, 0, set())


def vfs_view(name, memory, descriptor, budget, selectors=None):
    selectors = selectors or {}
    result = Result(source=number(descriptor, "root"), type_name="VFS namespace",
                    count_semantics="mount-aware supported backend contents or explicitly cached observations", units="sizes are bytes; modes are octal")
    try:
        stats = descriptor_root(memory, descriptor, "VfsDebugStats", 0, budget)
        if "--vfs" in selectors:
            owner = selectors["--vfs"]
            stats = memory.field(owner, "VirtualFileSystem", "DebugStats", budget)
            if number(stats, "source") != owner:
                raise ReadFailure("mismatched explicit VFS source")
        if name != "contents" and "--path" in selectors:
            raise ReadFailure("--path requires vfs contents")
        context = Namespace(memory, budget, result, stats, selectors.get("--scan", 4096))
        if name == "mounts":
            if not context.root:
                result.fail("namespace root not mounted")
            for address, mount in context.mounts.items():
                fs_address = number(mount, "filesystem")
                filesystem = context.read(fs_address, "filesystem_t") if fs_address else {"type": 0, "flags": 0, "rootInode": 0}
                fs_type = number(filesystem, "type")
                type_name = "unavailable (no type descriptor)" if not fs_type else context.string(number(context.read(fs_type, "filesystem_type_t"), "name"), 64)
                if not fs_address:
                    type_name = "unavailable (no filesystem record)"
                row(result, budget, context.mount_path(address), "mount=%#x parent=%#x point=%#x root=%#x filesystem=%#x type=%s mount flags=%#x filesystem flags=%#x root inode=%#x" % (
                    address, number(mount, "parentMount"), number(mount, "mountPoint"), number(mount, "root"), number(mount, "filesystem"),
                    type_name, number(mount, "flags"), number(filesystem, "flags"), number(filesystem, "rootInode")))
        elif name in ("cache", "contents"):
            entries = context.cache()
            if name == "cache":
                for pointer, source in entries + [(address, "retired") for address, _ in context.retired]:
                    value = context.dentry(pointer)
                    row(result, budget, source, "name=%r dentry=%#x parent=%#x inode=%#x unlinked=%s renamedTo=%#x; cached/retained only" % (
                        context.string(number(value, "name"), 256), pointer, number(value, "parent"), number(value, "inode"), bool(value["unlinked"]), number(value, "renamedTo")))
                    try:
                        row(result, budget, "cached ancestry", context.cached_path(pointer))
                    except ReadFailure as error:
                        result.fail(error)
            else:
                context.contents(selected_path(selectors))
                row(result, budget, "completeness", "complete for selected supported contents" if not result.issues else "incomplete; see reported bounds/backend/update issues")
        else:
            raise ReadFailure("unsupported VFS view")
    except (ReadFailure, ValueError, UnicodeError, OverflowError) as error:
        result.fail(error)
    return result


archive_view.selectors = ("--path", "--scan", "--initramfs")
vfs_view.selectors = ("--path", "--scan", "--vfs")
