#!/usr/bin/env python3
"""tcg-profile.py <dir> — the report behind tools/tcg-profile.sh (M9 track).

Reads <dir>/sample.txt (macOS `sample` call graph, 1 ms) and <dir>/perf.map
(QEMU -perfmap: `hostpc size guest-0x<pc>` per translated guest instruction)
and prints where the QEMU process spent its samples:

  * per thread (the vCPU thread is "CPU 0/TCG"), as % of the sampling wall time;
  * the vCPU thread's self time by bucket: generated code, i386 helpers (by
    family), the softmmu slow path (loads/stores that missed the TLB, page
    walks, MMIO), translation + TB lookup, interrupts/exceptions, waiting,
    other QEMU (top symbols listed);
  * generated code by guest region (XP kernel / hal / win32k / drivers /
    the EXE / user DLLs), the hottest 4 KiB guest pages and the hottest guest
    instructions — the input for a `-d in_asm,op_opt,out_asm -dfilter` run
    on those addresses (`--hot` prints a ready -dfilter list).

Self time = a node's count minus its children's. `sample` cannot unwind
through TCG-generated code, so generated-code samples appear as leaf
"???" frames directly under cpu_tb_exec / cpu_exec_loop, with their host
address; helpers called from generated code appear as their own leaves.
"""
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tcg_profile_lib import parse_sample, load_map, map_lookup, walk, vcpu_thread  # noqa: E402


BUCKETS = [  # (bucket, regex on the symbol), first match wins
    ('softmmu slow path', r'^(helper_(le|be|ret)?_?(ld|st)\w*_mmu|(load|store)_helper|do_(ld|st)\w*|mmu_lookup\w*|atomic_mmu_lookup|notdirty_write|tlb_set_dirty\w*|tlb_reset_dirty\w*|cpu_ld\w*|cpu_st\w*|tlb_fill\w*|x86_cpu_tlb_fill|mmu_translate|get_physical_address|tlb_set_page\w*|tlb_flush\w*|probe_access\w*|get_page_addr_code\w*|address_space_\w*|memory_region_\w*|io_(read|write)x|flatview_\w*|phys_page_find|cpu_physical_memory\w*|notdirty_write|iotlb_to_section|tlb_add_large_page|victim_tlb_hit)$'),
    ('translation + lookup', r'^(tb_gen_code|tb_lookup|helper_lookup_tb_ptr|tb_htable_lookup|tcg_gen_code|tcg_optimize|liveness_pass\w*|reachable_code_pass|tcg_reg_alloc\w*|tcg_out\w*|gen_intermediate_code|translator_loop|translate_insn|disas_insn\w*|decode_\w*|x86_translate_insn|tb_link_page|tb_add_jump|tcg_tb_insert|tcg_func_start|tcg_gen_\w*|gen_\w*|tcg_temp\w*|tcg_op_\w*|tcg_constant\w*|tcg_emit_op|tcg_target\w*|tb_page_addr\w*|page_\w*|tb_invalidate\w*|do_tb_phys_invalidate|tb_remove|tb_phys_invalidate\w*|tb_jmp_cache\w*|page_collection_\w*|page_trylock_add|page_lock\w*|page_unlock\w*|g_tree_\w*|DYLD-STUB\$\$g_tree_\w*|tb_page_addr_cmp|tb_tc_cmp|tcg_tb_lookup|tb_lookup_cmp|sys_icache_invalidate|i386_tr_\w*|init_ts_info|qht_\w*|tcg_tb_alloc|tcg_region\w*|tcg_flush_jmp_cache|tb_flush\w*|x86_ld_\w*|translator_ld\w*|ldub_code|tcg_\w*)$'),
    ('interrupts/exceptions', r'^(cpu_handle_interrupt|cpu_handle_exception|x86_cpu_exec_interrupt|x86_cpu_do_interrupt|do_interrupt\w*|raise_interrupt\w*|helper_raise\w*|cpu_loop_exit\w*|siglongjmp|_?_?sigsetjmp|_?_?setjmp|_?_?longjmp|cpu_get_pic_interrupt|apic_\w*|pic_\w*|cpu_interrupt\w*|x86_cpu_has_work|cpu_has_work|cpu_exec_enter|cpu_exec_exit|cpu_exec_longjmp_cleanup|x86_cpu_get_memory_mapping)$'),
    ('helpers: x87', r'^helper_(f\w*|x87\w*|shadow\w*)$'),
    ('helpers: sse/simd', r'^helper_(\w*(xmm|mmx)\w*|cvt\w*|comis\w*|ucomis\w*|movmsk\w*|sse\w*|simd\w*|enter_mmx|emms|ldmxcsr|update_mxcsr\w*|p\w+|rsqrt\w*|rcp\w*)$'),
    ('helpers: segments/paging', r'^helper_(load_seg|lcall\w*|ljmp\w*|lret\w*|iret\w*|sysenter|sysexit|syscall|sysret|ltr|lldt|verr|verw|lar|lsl|arpl|write_crN|read_crN|invlpg|wrmsr|rdmsr|cpuid|rdtsc\w*|rdpmc|hlt|monitor|mwait|pause|debug|clac|stac|set_dr|get_dr|into|bound|check_io\w*|in[bwl]|out[bwl]|clts|invd|wbinvd|rsm|flush_page|flush_tlb|cli|sti|rep\w*)$'),
    ('helpers: flags/cc', r'^helper_(cc_compute\w*|read_eflags|write_eflags|lock|unlock|bsf|bsr|pext|pdep|mulq\w*|imulq\w*|divl\w*|idivl\w*|divw\w*|idivw\w*|divb\w*|idivb\w*|divq\w*|idivq\w*|aam|aad|aaa|aas|daa|das|bswap\w*|cmpxchg\w*|rcl\w*|rcr\w*|rot\w*|shl\w*|shr\w*|sar\w*)$'),
    ('helpers: other', r'^helper_\w+$'),
    ('perf map writer', r'^(__vfprintf|__sfvwrite|__write_nocancel|_?_?fprintf|__v2printf|__ultoa|__sprint|_?flockfile|_?funlockfile|__swrite|_?_?write)$'),
    ('waiting', r'^(__psynch\w*|_?pthread_cond\w*|_?pthread_mutex\w*|qemu_mutex_\w*|qemu_cond_\w*|__semwait\w*|semaphore_\w*|kevent\w*|_?_?poll|__select|mach_msg\w*|__ulock\w*|qemu_futex\w*|futex_wait|nanosleep|__sleep|__workq_kernreturn|start_wqthread|_dispatch_workloop_worker_thread|qemu_poll_ns|g_poll|ppoll|os_event_wait|qemu_event_wait|bql_lock|bql_unlock|qemu_cpu_kick\w*|thread_start|_pthread_start|qemu_thread_start|rcu_\w*)$'),
]
# the plain "guest-0x..." leaf list and its per-region classification
REGIONS = [  # (name, lo, hi) for an XP SP3 32-bit guest; pc-ranges, first match wins
    ('kernel: ntoskrnl', 0x80400000, 0x80800000),
    ('kernel: win32k', 0xbf800000, 0xbfa00000),
    ('kernel: drivers/other', 0x80000000, 0xffffffff),
    ('user: exe (image base 0x400000)', 0x00400000, 0x10000000),
    ('user: real mode / low', 0x00000000, 0x00400000),
    ('user: dlls', 0x10000000, 0x80000000),
]


def region_of(pc):
    for name, lo, hi in REGIONS:
        if lo <= pc <= hi:
            return name
    return 'unknown'


def bucket_of(sym):
    for b, rx in BUCKETS:
        if re.match(rx, sym):
            return b
    return 'other'



def pct(n, tot):
    return f'{100.0 * n / tot:5.1f}%' if tot else '   - '


def main():
    d = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else '.'
    hot = '--hot' in sys.argv
    interval, threads = parse_sample(os.path.join(d, 'sample.txt'))
    mp = load_map(os.path.join(d, 'perf.map')) if os.path.exists(os.path.join(d, 'perf.map')) else ([], [], [], None, None)
    if not threads:
        print('no call graph in sample.txt'); return 1
    total = sum(t.n for t in threads)
    # wall time of the sampling: from the header "Sampling process N for S seconds"
    secs = None
    with open(os.path.join(d, 'sample.txt'), errors='replace') as f:
        for line in f:
            m = re.match(r'Sampling process \d+ for (\d+) seconds', line)
            if m:
                secs = int(m.group(1)); break
    wall = secs * 1000.0 / interval if secs else None

    print(f'== {os.path.basename(os.path.abspath(d))}: {total} samples at {interval:g} ms'
          + (f', {secs} s wall ({pct(total, wall)} of one core)' if wall else ''))
    print('\n-- threads (samples = ms of CPU)')
    vcpu = vcpu_thread(threads)
    if vcpu is None:
        vcpu = max(threads, key=lambda t: t.n)
        print(f'  (no thread runs cpu_exec_loop; using the busiest, {vcpu.sym})')
    for t in sorted(threads, key=lambda t: -t.n):
        print(f'  {t.n:7d} {pct(t.n, wall) if wall else ""} {t.sym}{"  <- vCPU thread" if t is vcpu else ""}')

    # --- vCPU thread buckets from self counts
    buckets = defaultdict(int)
    syms = defaultdict(int)
    gen = defaultdict(int)       # guest pc -> samples
    gen_unmapped = 0
    def leaf(n):
        if n.self <= 0:
            return
        if n.sym.startswith('???') or n.bin in (None, '<unknown binary>'):
            name = map_lookup(mp, int(n.addr, 16)) if n.addr else None
            if name and name.startswith('guest-0x'):
                buckets['generated code'] += n.self
                gen[int(name[6:], 16)] += n.self
            elif name in ('guest-?', 'tcg-prologue-buffer'):
                buckets['generated code'] += n.self
                nonlocal_unmapped[0] += n.self
            else:
                buckets['other'] += n.self
                syms[f'??? {n.addr} ({n.bin})'] += n.self
            return
        b = bucket_of(n.sym)
        buckets[b] += n.self
        if b != 'generated code':
            syms[f'{n.sym}  [{b}]'] += n.self
    nonlocal_unmapped = [0]
    walk(vcpu, leaf)
    gen_unmapped = nonlocal_unmapped[0]
    vt = vcpu.n
    print(f'\n-- vCPU thread self time ({vt} samples)')
    for b, n in sorted(buckets.items(), key=lambda kv: -kv[1]):
        extra = f'  ({gen_unmapped} in the code buffer with no map entry)' if b == 'generated code' and gen_unmapped else ''
        print(f'  {n:7d} {pct(n, vt)} {b}{extra}')
    print('\n-- top symbols outside generated code (vCPU thread)')
    for s, n in sorted(syms.items(), key=lambda kv: -kv[1])[:40]:
        print(f'  {n:7d} {pct(n, vt)} {s}')

    # --- generated code by guest region / page / instruction
    gtot = sum(gen.values())
    if gtot:
        regions = defaultdict(int); pages = defaultdict(int)
        for pc, n in gen.items():
            regions[region_of(pc)] += n
            pages[pc & ~0xfff] += n
        print(f'\n-- generated code by guest region ({gtot} mapped samples)')
        for r, n in sorted(regions.items(), key=lambda kv: -kv[1]):
            print(f'  {n:7d} {pct(n, gtot)} {r}')
        print('\n-- hottest guest pages (4 KiB)')
        for p, n in sorted(pages.items(), key=lambda kv: -kv[1])[:24]:
            print(f'  {n:7d} {pct(n, gtot)} 0x{p:08x}  {region_of(p)}')
        print('\n-- hottest guest instructions')
        for pc, n in sorted(gen.items(), key=lambda kv: -kv[1])[:40]:
            print(f'  {n:7d} {pct(n, gtot)} 0x{pc:08x}')
        if hot:
            tops = sorted(pages.items(), key=lambda kv: -kv[1])[:8]
            print('\n-- -dfilter for the hottest pages:')
            print('  -d in_asm,op_opt,out_asm -dfilter ' + ','.join(f'0x{p:x}..0x{p + 0x1000:x}' for p, _ in tops))

    # --- other threads: their top self symbols (what the rest of QEMU is doing)
    print('\n-- other threads, top self symbols')
    for t in sorted(threads, key=lambda t: -t.n):
        if t is vcpu or t.n < total * 0.01:
            continue
        s2 = defaultdict(int)
        walk(t, lambda n: s2.__setitem__(n.sym, s2[n.sym] + n.self) if n.self > 0 else None)
        top = sorted(s2.items(), key=lambda kv: -kv[1])[:5]
        print(f'  {t.sym}: ' + ', '.join(f'{k} {v}' for k, v in top))
    return 0


if __name__ == '__main__':
    sys.exit(main())
