"""Candidate discovery for the October 2026 readability tickets.

This is analysis tooling, not a C/C++ parser or a readability acceptance test.
Comments, literals, and preprocessor continuations are masked for control scans.
Spacing spans require human review; grouped records and tables may stay dense.
Exit 1 means candidates exist, and exit 0 means none were found.
"""

import bisect
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXCLUDED = ('/uACPI/', '/flanterm/', '/printf/', '/khashl/')
LITERALS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"([^ ()\\\t\n]{0,16})\([\s\S]*?\)\1"|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')
TOKEN = re.compile(r'\b[a-zA-Z_]\w*\b|[^\s]')


def mask(source):
    source = LITERALS.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()), source)
    lines = source.splitlines(keepends=True)
    continuation = False
    for index, line in enumerate(lines):
        directive = continuation or line.lstrip().startswith('#')
        continuation = directive and line.rstrip().endswith('\\')
        if directive:
            lines[index] = re.sub(r'[^\n]', ' ', line)
    return ''.join(lines)


def sources():
    for directory in ('arch', 'kernel', 'klib', 'boot', 'initramfs'):
        for path in sorted((ROOT / directory).rglob('*')):
            relative = path.relative_to(ROOT).as_posix()
            if path.suffix not in ('.c', '.cpp', '.h', '.hpp'):
                continue
            if any(part in '/' + relative for part in EXCLUDED) or relative == 'boot/limine.h':
                continue
            yield path


def controls(path):
    source = path.read_text()
    clean = mask(source)
    tokens = list(TOKEN.finditer(clean))
    values = [token.group() for token in tokens]
    newlines = [match.start() for match in re.finditer('\n', source)]
    for index, value in enumerate(values):
        if value not in ('if', 'for', 'while', 'else', 'do'):
            continue
        end = index + 1
        if value in ('if', 'for', 'while'):
            if values[end] == 'constexpr':
                end += 1
            if values[end] != '(':
                continue
            depth = 1
            end += 1
            while end < len(values) and depth:
                depth += (values[end] == '(') - (values[end] == ')')
                end += 1
            if value == 'while' and values[end] == ';' and values[index - 1] == '}':
                continue
        if values[end] == '{' or (value == 'else' and values[end] == 'if'):
            continue
        line = bisect.bisect_right(newlines, tokens[index].start()) + 1
        yield line, source.splitlines()[line - 1].strip()


def dense(path):
    lines = path.read_text().splitlines()
    start = 0
    for index, line in enumerate(lines + ['']):
        if not line.strip():
            if index - start >= 25:
                yield start + 1, index
            start = index + 1


def examples():
    cases = (
        (
            'kernel/layers/Logic/SynchronizationManager.cpp',
            'uint64_t SynchronizationManager::PriorityFutex',
            '\nvoid SynchronizationManager::CleanupPriority',
        ),
        (
            'kernel/layers/Logic/SchedulerPolicies.cpp',
            'uint64_t Scheduler::ConfigurePolicy',
            '\nuint64_t Scheduler::ReadPolicy',
        ),
        (
            'kernel/layers/Logic/ProcessLifecycle.cpp',
            '    // Siblings have retired.',
            '\n    {\n        SignalDispositionGuard guard(current);',
        ),
    )
    failures = 0

    for filename, start, end in cases:
        source = (ROOT / filename).read_text()
        region = source.split(start, 1)[1].split(end, 1)[0].strip('\n')
        lines = region.splitlines()
        boundaries = sum(not line.strip() for line in lines)
        failures += boundaries == 0
        verdict = 'FAIL' if boundaries == 0 else 'REVIEW'
        print(f'{verdict}: {filename}: {len(lines)} lines, {boundaries} blank lines')

    # Separation existing is necessary but insufficient; a human reviews phases.
    return 1 if failures else 0


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ('braces', 'spacing', 'examples'):
        raise SystemExit('usage: readability_audit.py braces|spacing|examples')

    mode = sys.argv[1]

    if mode == 'examples':
        return examples()
    count = 0
    files = 0

    for path in sources():
        found = list(controls(path) if mode == 'braces' else dense(path))

        if found:
            files += 1
            count += len(found)
            print(path.relative_to(ROOT), found)

    print(f'{mode}: {count} candidates in {files} files')
    return 1 if count else 0


if __name__ == '__main__':
    raise SystemExit(main())
