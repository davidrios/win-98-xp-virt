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


def load_map(path, epoch=None):
    """The -perfmap file, restricted to one *epoch* of the code buffer.

    TCG bump-allocates TBs from the code buffer and every tb_flush resets the
    pointer, so the file is a sequence of epochs whose host ranges overlap:
    the same host address holds a different TB in every epoch, and a lookup
    that merges them attributes a sample to whichever entry happens to start
    nearest (a 2026-09-05 lesson: a game's hottest instruction came out as a
    different guest address in every run).  An epoch starts where the start
    addresses jump back; `epoch` picks one (0 = boot; None = the last).
    Returns (starts, ends, names, lo, hi, n_epochs)."""
    epochs = [{}]
    prev = 0
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 3:
                continue
            start, size, name = int(parts[0], 16), int(parts[1], 16), parts[2]
            if name == 'tcg-prologue-buffer':
                epochs[0][start] = (size, name)   # written once, valid in every epoch
                continue
            if start < prev - (1 << 20):
                epochs.append({})
            prev = start
            epochs[-1][start] = (size, name)
    n = len(epochs)
    if epoch is None or epoch >= n:
        epoch = n - 1
    entries = dict(epochs[epoch])
    entries.update({k: v for k, v in epochs[0].items() if v[1] == 'tcg-prologue-buffer'})
    starts, ends, names = [], [], []
    for s in sorted(entries):
        size, name = entries[s]
        starts.append(s); ends.append(s + size); names.append(name)
    lo = hi = None
    if starts:
        lo, hi = starts[0], max(ends)
    return starts, ends, names, lo, hi, n


def sample_epochs(d):
    """(before, after): the `info jit` TB flush counts tcg-profile.sh saved
    around the sample, i.e. the code-buffer epoch(s) the sample window was in;
    None where the file is missing."""
    import os
    r = []
    for name in ('info-jit-before.txt', 'info-jit-after.txt'):
        p = os.path.join(d, name)
        m = None
        if os.path.exists(p):
            m = re.search(r'TB flush count\s+(\d+)', open(p, errors='replace').read())
        r.append(int(m.group(1)) if m else None)
    return r[0], r[1]


def load_map_for(d):
    """The map for a run directory: the epoch of its sample window.  When the
    window straddled a flush the epoch with more entries covering the
    samples cannot be known here; the later one is used and the note says so."""
    import os
    path = os.path.join(d, 'perf.map')
    if not os.path.exists(path):
        return ([], [], [], None, None, 0), ''
    before, after = sample_epochs(d)
    epoch = after if after is not None else before
    mp = load_map(path, epoch)
    n = mp[5]
    if epoch is None:
        note = f'perf map: {n} epoch(s), no info-jit flush count: using the last'
    elif before != after:
        note = (f'perf map: the sample straddled a tb_flush (epoch {before} -> {after} of {n}); '
                f'attributing to epoch {epoch} - samples before the flush are mismapped')
    else:
        note = f'perf map: epoch {epoch} of {n}' if n > 1 else ''
    return mp, note


def map_lookup(mp, addr):
    starts, ends, names, lo, hi = mp[:5]
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
