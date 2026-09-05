"""Shared by tools/tcg-profile.py and tools/tcg-hot.py (M9 track): the macOS
`sample` call-graph parser and the QEMU -perfmap lookup."""
import bisect
import re


LINE = re.compile(r'^(?P<pre>[\s+!:|]*?)(?P<n>\d+) (?P<rest>.*)$')


class Node:
    __slots__ = ('n', 'sym', 'bin', 'addr', 'kids', 'depth', 'self')

    def __init__(self, n, sym, bin_, addr, depth):
        self.n, self.sym, self.bin, self.addr, self.depth = n, sym, bin_, addr, depth
        self.kids = []
        self.self = n


def parse_sample(path):
    """-> (interval_ms, [thread roots])"""
    threads = []
    stack = []
    interval = 1.0
    in_graph = False
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            m = re.match(r'Sampling process \d+ for \d+ seconds with (\d+) millisecond', line)
            if m:
                interval = float(m.group(1))
            if line.startswith('Call graph:'):
                in_graph = True
                continue
            if not in_graph:
                continue
            if not line.strip():
                if stack:
                    in_graph = False
                continue
            if line.startswith('Total number in stack') or line.startswith('Sort by top of stack'):
                break
            m = LINE.match(line)
            if not m:
                continue
            depth = len(m.group('pre'))
            rest = m.group('rest')
            am = re.search(r'\[(0x[0-9a-f]+)\]', rest)
            bm = re.search(r'\(in ([^)]*)\)', rest)
            sym = re.sub(r' \+ \d+$', '', rest.split('  (in ')[0].split('  [')[0].strip())
            node = Node(int(m.group('n')), sym, bm.group(1) if bm else None, am.group(1) if am else None, depth)
            while stack and stack[-1].depth >= depth:
                stack.pop()
            if stack:
                stack[-1].kids.append(node)
            else:
                threads.append(node)
            stack.append(node)
    def fix_self(n):
        for k in n.kids:
            fix_self(k)
        n.self = n.n - sum(k.n for k in n.kids)
    for t in threads:
        fix_self(t)
    return interval, threads


def load_map(path):
    starts, ends, names = [], [], []
    entries = {}
    lo = hi = None
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 3:
                continue
            start, size, name = int(parts[0], 16), int(parts[1], 16), parts[2]
            entries[start] = (size, name)  # later entries (after a tb flush) win
    for s in sorted(entries):
        size, name = entries[s]
        starts.append(s); ends.append(s + size); names.append(name)
    if starts:
        lo, hi = starts[0], max(ends)
    return starts, ends, names, lo, hi


def map_lookup(mp, addr):
    starts, ends, names, lo, hi = mp
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and addr < ends[i]:
        return names[i]
    if lo is not None and lo <= addr < hi:
        return 'guest-?'   # inside the code buffer, no entry (stale after a flush, or the prologue/epilogue)
    return None



def walk(node, fn):
    fn(node)
    for k in node.kids:
        walk(k, fn)


def vcpu_thread(threads):
    """the thread with the most samples under cpu_exec_loop (threads are unnamed in `sample`)"""
    def under(n, sym):
        return n.n if n.sym == sym else sum(under(k, sym) for k in n.kids)
    vcpu = max(threads, key=lambda t: under(t, 'cpu_exec_loop'))
    return vcpu if under(vcpu, 'cpu_exec_loop') else None
