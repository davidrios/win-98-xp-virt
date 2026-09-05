#!/usr/bin/env python3
"""tcg-hot.py <profile dir> [--dlog <qemu-d.log>] [-n N]

Second pass of tools/tcg-profile.sh (M9 track): what the host code of the
*hot* guest instructions is made of. Needs the first pass's sample.txt and
perf.map (samples per guest instruction, host bytes per guest instruction)
plus a `-d in_asm,op_opt,out_asm -dfilter ...` log of the same workload
(DFILTER= on a second tcg-profile.sh run; default <dir>/qemu-d.log) and
prints, weighted by samples:

  * host instructions per guest instruction, and their class mix (loads /
    stores / branches / ALU / SIMD / barriers — decoded from the aarch64
    opcode fields, no disassembler needed);
  * the TCG op mix per guest instruction: guest memory accesses (qemu_ld /
    qemu_st = a softmmu TLB lookup each), condition-code traffic (cc_dst /
    cc_src / cc_op reads and writes), helper calls, barriers, the rest;
  * the guest instruction mix (mnemonics; needs the `capstone` module —
    `uv venv v && uv pip install --python v/bin/python capstone`, run with
    v/bin/python — else the bytes are printed);
  * the N hottest guest instructions with all of the above.

Guest instruction = one `---- pc cs_base flags` section of the op dump and
one `-- guest addr` section of the host dump; linear address = the TB's
IN address + (pc - first pc). Coverage says how many of the profile's
samples fall on instructions the log covers.
"""
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tcg_profile_lib import parse_sample, load_map, map_lookup, vcpu_thread, walk  # noqa: E402

try:
    import capstone
    CS = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
except Exception:  # pragma: no cover
    CS = None


def host_class(word):
    """aarch64 instruction class from the encoding (A64 op0 field, bits 28:25)."""
    if word & 0xfffff0ff == 0xd50330bf:
        return 'barrier'
    op0 = (word >> 25) & 0xf
    if op0 & 0b0101 == 0b0100:
        # load/store: distinguish by the L bit where it exists (bit 22 for
        # most register/immediate forms; ldp/stp use bit 22 too)
        return 'load' if (word >> 22) & 1 else 'store'
    if op0 & 0b1110 == 0b1010:
        return 'branch'
    if op0 & 0b1110 == 0b1000:
        return 'alu'
    if op0 & 0b0111 == 0b0101:
        return 'alu'
    if op0 & 0b0111 == 0b0111:
        return 'simd'
    return 'other'


ENV_OFF = {0x20: 'eip update', 0x24: 'eflags', 0x34: 'cc_op store', 0x28: 'cc_dst/src', 0x2c: 'cc_dst/src', 0x30: 'cc_dst/src'}


def tag_host(words):
    """Per host instruction of one guest instruction: what it is for.
    Recognises the aarch64 softmmu fast path (ldp x16,x17,[x19,#-off] ... b.cond,
    then the access), env traffic by offset (x19 = env), block exits."""
    tags = ['work'] * len(words)
    state = None
    for i, w in enumerate(words):
        if w & 0xfffff0ff == 0xd50330bf:
            tags[i] = 'barrier'; continue
        rn = (w >> 5) & 0x1f
        if (w & 0xffc00000) in (0xa9400000, 0xa9700000, 0xa9600000, 0xa9500000) and rn == 19 and (w & 0x1f) == 16 and ((w >> 10) & 0x1f) == 17:
            state = 'tlb'; tags[i] = 'tlb lookup'; continue          # ldp x16, x17, [x19, #tlb]
        if state == 'tlb':
            tags[i] = 'tlb lookup'
            if (w & 0xff000010) == 0x54000000:                        # b.cond -> slow path
                state = 'access'
            continue
        if state == 'access':
            if (w >> 25) & 0b0101 == 0b0100:                          # the load/store itself
                tags[i] = 'guest access'; state = None; continue
            state = None
        if (w & 0xbfc00000) in (0xb9400000, 0xb9000000, 0xb8400000, 0xb8000000, 0x39400000, 0x39000000, 0x79400000, 0x79000000) and rn == 19:
            off = ((w >> 10) & 0xfff) * (4 if (w >> 30) & 1 else 1)  # unsigned-offset ld/st via env
            tags[i] = ENV_OFF.get(off, 'guest regs' if off < 0x20 else 'env other'); continue
        if (w & 0xffc00000) == 0xf9400000 and rn == 19:
            tags[i] = 'env other'; continue                           # 64-bit ldr from env
        if (w & 0xfc000000) == 0x14000000 or (w & 0xfffffc1f) == 0xd61f0000 or (w & 0x9f000000) in (0x90000000, 0x10000000) or (w & 0xffe0001f) == 0x91000010:
            tags[i] = 'tb exit'; continue                             # b, br x16, adr/adrp, add x16 (goto_tb / exit_tb)
        if (w & 0xfffffc1f) == 0xd63f0000:
            tags[i] = 'slow path stub'; continue                      # blr: helper call
    return tags


def parse_dlog(path):
    """-> {linear pc: {'ops': [op lines], 'host': [words], 'bytes': (tb_bytes, offset)}}"""
    insns = {}
    tb_start = None
    with open(path, errors='replace') as f:
        txt = f.read()
    for block in txt.split('----------------\n'):
        if 'OP after optimization' not in block or 'OUT:' not in block:
            continue
        in_part, rest = block.split('OP after optimization', 1)
        op_part, out_part = rest.split('OUT:', 1)
        m = re.search(r'^0x([0-9a-f]+):', in_part, re.M)
        if not m:
            continue
        lin0 = int(m.group(1), 16)
        tb_bytes = bytes.fromhex(''.join(re.findall(r'^OBJD-T: ([0-9a-f]+)', in_part, re.M)))
        # ops per guest instruction
        secs = re.split(r'^ ---- ([0-9a-f]+) [0-9a-f]+ [0-9a-f]+\s*$', op_part, flags=re.M)
        pcs = [int(p, 16) for p in secs[1::2]]
        ops = [[l.strip() for l in s.splitlines() if l.strip()] for s in secs[2::2]]
        if not pcs:
            continue
        pc0 = pcs[0]
        # host words per guest instruction
        hsecs = re.split(r'^  -- guest addr 0x([0-9a-f]+).*$', out_part, flags=re.M)
        hpcs = [int(p, 16) for p in hsecs[1::2]]
        hwords = []; hstarts = []
        for s in hsecs[2::2]:
            hm = re.search(r'^0x([0-9a-f]+):', s, re.M)
            hstarts.append(int(hm.group(1), 16) if hm else None)
            hexs = ''.join(re.findall(r'^OBJD-H: ([0-9a-f]+)', s, re.M))
            b = bytes.fromhex(hexs)
            hwords.append([int.from_bytes(b[i:i + 4], 'little') for i in range(0, len(b) - 3, 4)])
        host_by_pc = dict(zip(hpcs, zip(hwords, hstarts)))
        tb_tag = 'chained' if ' goto_tb ' in op_part else 'unchained'
        for pc, oplist in zip(pcs, ops):
            lin = lin0 + (pc - pc0)
            words, hstart = host_by_pc.get(pc, ([], None))
            info = {'pc': lin, 'ops': oplist, 'host': words, 'bytes': (tb_bytes, pc - pc0), 'tb': tb_tag, 'hstart': hstart}
            # keyed by the host address of the instruction's code: the same guest
            # instruction is translated several times (TB boundaries, flags) and
            # the samples identify one translation
            if hstart is not None:
                insns[hstart] = info
            insns.setdefault(lin, info)   # by guest pc: the first translation, for --insn
    return insns


def op_kind(op):
    name = op.split()[0]
    if name.startswith('qemu_ld'):
        return 'mem load'
    if name.startswith('qemu_st'):
        return 'mem store'
    if name == 'mb':
        return 'barrier'
    if name == 'call':
        return 'helper call'
    if re.search(r'\bcc_(dst|src|src2|op)\b', op) or re.search(r'env,\$0x34\b', op):
        return 'cc traffic'
    if name in ('set_label', 'br', 'brcond_i32', 'exit_tb', 'goto_tb', 'goto_ptr', 'discard'):
        return 'control'
    if re.search(r'\benv,', op):
        return 'env ld/st'
    return 'alu/mov'


def guest_text(info):
    tb_bytes, off = info['bytes']
    if CS is None or off >= len(tb_bytes):
        return tb_bytes[off:off + 8].hex() if off < len(tb_bytes) else '?'
    for i in CS.disasm(tb_bytes[off:off + 16], 0):
        return f'{i.mnemonic} {i.op_str}'.strip()
    return tb_bytes[off:off + 8].hex()


def main():
    args = sys.argv[1:]
    d = args[0]
    dlog = os.path.join(d, 'qemu-d.log')
    n_top = 40
    if '--dlog' in args:
        dlog = args[args.index('--dlog') + 1]
    if '-n' in args:
        n_top = int(args[args.index('-n') + 1])
    if '--insn' in args:   # dump one guest instruction: its TCG ops and host code classes
        pc = int(args[args.index('--insn') + 1], 16)
        allinfo = parse_dlog(dlog)
        variants = {id(i): i for i in allinfo.values() if i['pc'] == pc}
        if not variants:
            print('not in the log'); return 1
        for info in variants.values():   # every translation of that guest instruction
            print(f'0x{pc:08x}  {guest_text(info)}   [{info["tb"]}, host code at 0x{info["hstart"] or 0:x}]')
            for o in info['ops']:
                print(f'   {op_kind(o):12s} {o}')
            print(f'  host ({len(info["host"])} instructions):')
            print('   ' + ' '.join(f'{host_class(w)}:{w:08x}' for w in info['host']))
        return 0
    _, threads = parse_sample(os.path.join(d, 'sample.txt'))
    mp = load_map(os.path.join(d, 'perf.map'))
    vcpu = vcpu_thread(threads)
    samples = defaultdict(int)
    host_bytes = {}
    starts, ends, names, _, _ = mp
    for s, e, nm in zip(starts, ends, names):
        if nm.startswith('guest-0x'):
            host_bytes[int(nm[6:], 16)] = e - s
    gen_total = [0]

    import bisect
    def leaf(n):   # samples keyed by the host start address of the guest instruction's code
        if n.self > 0 and n.addr and (n.sym.startswith('???') or n.bin == '<unknown binary>'):
            a = int(n.addr, 16)
            i = bisect.bisect_right(starts, a) - 1
            if i >= 0 and a < ends[i] and names[i].startswith('guest-0x'):
                samples[starts[i]] += n.self
                gen_total[0] += n.self
    walk(vcpu, leaf)
    insns = parse_dlog(dlog)
    # per-sample host instruction tags: the exact instruction each sample hit
    by_host = {}
    for h, info in insns.items():
        if info['hstart'] is not None and h == info['hstart']:
            for i, t in enumerate(tag_host(info['host'])):
                by_host[info['hstart'] + 4 * i] = (t, host_class(info['host'][i]))
    tagged = defaultdict(int); cls_exact = defaultdict(int); n_exact = 0
    def leaf2(n):
        nonlocal n_exact
        if n.self > 0 and n.addr and (n.sym.startswith('???') or n.bin == '<unknown binary>'):
            hit = by_host.get(int(n.addr, 16))
            if hit:
                tagged[hit[0]] += n.self; cls_exact[hit[1]] += n.self; n_exact += n.self
    walk(vcpu, leaf2)
    if n_exact:
        print(f'\n-- where the samples land inside generated code ({n_exact} samples at an exact host instruction)')
        for k, v in sorted(tagged.items(), key=lambda kv: -kv[1]):
            print(f'  {100.0 * v / n_exact:5.1f}%  {k}')
        print('   by class: ' + ', '.join(f'{k} {100.0 * v / n_exact:.1f}%' for k, v in sorted(cls_exact.items(), key=lambda kv: -kv[1])))
    covered = {h: s for h, s in samples.items() if h in insns}
    cov = sum(covered.values())
    print(f'== {os.path.basename(os.path.abspath(d))}: {gen_total[0]} generated-code samples, '
          f'{cov} ({100.0 * cov / max(gen_total[0], 1):.1f}%) on the {len(insns)} guest instructions the log covers')
    if not cov:
        return 1

    hcls = defaultdict(float); okind = defaultdict(float); mnem = defaultdict(float)
    w_host = 0.0; w_ops = 0.0; w_mem_insns = 0.0; w_cc_insns = 0.0; w_call_insns = 0.0
    tbkind = defaultdict(float)
    for h, s in covered.items():
        info = insns[h]
        words = info['host']
        nh = len(words) if words else host_bytes.get(info['pc'], 0) // 4
        tbkind[info['tb']] += s
        w_host += s * nh
        for w in words:
            hcls[host_class(w)] += s
        kinds = [op_kind(o) for o in info['ops']]
        w_ops += s * len(kinds)
        for k in kinds:
            okind[k] += s
        if any(k.startswith('mem') for k in kinds):
            w_mem_insns += s
        if 'cc traffic' in kinds:
            w_cc_insns += s
        if 'helper call' in kinds:
            w_call_insns += s
        mnem[guest_text(info).split(' ')[0]] += s

    print(f'\n-- samples by translation kind: ' + ', '.join(f'{k} {100.0 * v / cov:.1f}%' for k, v in sorted(tbkind.items())))
    print(f'\n-- per guest instruction, weighted by samples')
    print(f'  host instructions      {w_host / cov:6.1f}')
    print(f'  TCG ops                {w_ops / cov:6.1f}')
    print(f'  with a memory access   {100.0 * w_mem_insns / cov:5.1f}%   (a softmmu TLB lookup each)')
    print(f'  with cc traffic        {100.0 * w_cc_insns / cov:5.1f}%')
    print(f'  with a helper call     {100.0 * w_call_insns / cov:5.1f}%')
    tot = sum(hcls.values())
    if tot:
        print(f'\n-- host instruction classes (of {w_host / cov:.1f} per guest instruction)')
        for k, v in sorted(hcls.items(), key=lambda kv: -kv[1]):
            print(f'  {100.0 * v / tot:5.1f}%  {v / cov:5.1f}/insn  {k}')
    tot = sum(okind.values())
    print(f'\n-- TCG op kinds (of {w_ops / cov:.1f} per guest instruction)')
    for k, v in sorted(okind.items(), key=lambda kv: -kv[1]):
        print(f'  {100.0 * v / tot:5.1f}%  {v / cov:5.2f}/insn  {k}')
    print(f'\n-- guest mnemonics')
    for k, v in sorted(mnem.items(), key=lambda kv: -kv[1])[:25]:
        print(f'  {100.0 * v / cov:5.1f}%  {k}')
    print(f'\n-- hottest {n_top} guest instructions: samples, host insns, ops (mem/cc/call), text')
    for h, s in sorted(covered.items(), key=lambda kv: -kv[1])[:n_top]:
        info = insns[h]; pc = info['pc']
        kinds = [op_kind(o) for o in info['ops']]
        nm = sum(k.startswith('mem') for k in kinds); nc = kinds.count('cc traffic'); nl = kinds.count('helper call')
        calls = ','.join(o.split()[1].split(',')[0] for o in info['ops'] if o.startswith('call'))
        print(f'  {s:6d} {100.0 * s / cov:5.1f}%  0x{pc:08x}  host {len(info["host"]):3d}  ops {len(kinds):2d} ({nm}/{nc}/{nl}{" " + calls if calls else ""})  {info["tb"][:2]}  {guest_text(info)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
