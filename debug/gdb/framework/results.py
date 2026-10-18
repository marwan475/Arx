"""Target-independent bounded reader results. No formatting or target execution."""
from dataclasses import dataclass, field


class ReadFailure(Exception):
    pass


@dataclass
class Budget:
    entries: int = 100
    bytes: int = 65536
    depth: int = 8
    used_entries: int = 0
    used_bytes: int = 0

    def take_entry(self):
        if self.used_entries >= self.entries:
            raise ReadFailure("truncated: entry limit")
        self.used_entries += 1

    def take_bytes(self, size):
        if size < 0 or size > self.bytes - self.used_bytes:
            raise ReadFailure("truncated: byte limit")
        self.used_bytes += size


@dataclass
class Result:
    items: list = field(default_factory=list)
    issues: list = field(default_factory=list)
    source: int = 0
    type_name: str = ""
    count_semantics: str = "observed entries, not an atomic snapshot"
    units: str = "provider-defined; see semantic contract"

    @property
    def status(self):
        return "partial" if self.issues else "complete"

    def fail(self, reason):
        if str(reason) not in self.issues:
            self.issues.append(str(reason))
        return self
