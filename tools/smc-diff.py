#!/usr/bin/env python3
"""smc-diff.py <mem-A.bin> <mem-B.bin> <guest address> — which instructions a
guest patches in its own code (M9 track: the self-modifying rasterizers).

Two captures of the same guest-virtual range taken a moment apart
(`RACE_MEMSAVE=` in tools/xp-moto-race.sh, or QMP `memsave`) are compared
byte by byte; every run of differing bytes is reported with the x86
instruction that contains it (capstone, `build/venv-capstone/bin/python`)
and which of its bytes changed — an immediate, a displacement, an opcode.
Prints a summary: patched instructions, bytes, and the patch kinds.

    build/venv-capstone/bin/python tools/smc-diff.py \\
        build/tcg-profile/<name>/race/mem-0x482000-a.bin \\
        build/tcg-profile/<name>/race/mem-0x482000-b.bin 0x482000
"""
import sys

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM
except ImportError:
    raise SystemExit("needs capstone: build/venv-capstone/bin/python tools/smc-diff.py …")


def main():
    a = open(sys.argv[1], "rb").read()
    b = open(sys.argv[2], "rb").read()
    base = int(sys.argv[3], 16)
    if len(a) != len(b):
        raise SystemExit("captures differ in size (%d vs %d)" % (len(a), len(b)))
    diff = [i for i in range(len(a)) if a[i] != b[i]]
    print("%d of %d bytes differ" % (len(diff), len(a)))
    if not diff:
        return 0
    # runs of differing bytes
    runs, start = [], None
    for i, off in enumerate(diff):
        if start is None:
            start = off
        if i + 1 == len(diff) or diff[i + 1] != off + 1:
            runs.append((start, off))
            start = None
    print("%d patched runs" % len(runs))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    # decode the whole capture linearly from base (the pages hold straight-line
    # code; a run that lands mid-instruction is reported as such)
    insns = {}
    for ins in md.disasm(b, base):
        insns[ins.address] = ins
    starts = sorted(insns)
    kinds = {}
    shown = 0
    for lo, hi in runs:
        addr = base + lo
        # the instruction containing the run's first byte
        ins = None
        for s in reversed(starts):
            if s <= addr:
                ins = insns[s]
                break
        if ins is None or addr >= ins.address + ins.size:
            kind = "outside decoded code"
        else:
            kind = "opcode"
            for op in ins.operands:
                if op.type == X86_OP_IMM and ins.imm_offset and \
                        ins.address + ins.imm_offset <= addr < ins.address + ins.imm_offset + ins.imm_size:
                    kind = "imm%d" % (8 * ins.imm_size)
                elif op.type == X86_OP_MEM and ins.disp_offset and \
                        ins.address + ins.disp_offset <= addr < ins.address + ins.disp_offset + ins.disp_size:
                    kind = "disp%d" % (8 * ins.disp_size)
        kinds[kind] = kinds.get(kind, 0) + 1
        if shown < 40:
            shown += 1
            old = a[lo:hi + 1].hex()
            new = b[lo:hi + 1].hex()
            if ins is not None:
                print("  %08x+%d..%d %-7s %s %s   [%s -> %s]" % (
                    ins.address, addr - ins.address, hi - lo + 1 + addr - ins.address - 1,
                    kind, ins.mnemonic, ins.op_str, old, new))
            else:
                print("  %08x %-7s [%s -> %s]" % (addr, kind, old, new))
    print("patch kinds: " + ", ".join("%s %d" % kv for kv in sorted(kinds.items(), key=lambda kv: -kv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
