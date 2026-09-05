# Track: M9 — TCG on Apple Silicon: where the vCPU's time goes, then the fix

The handoff for a session that makes the emulated CPU run faster on the
Air (aarch64 host, TCG only — there is no accelerator for an x86 guest on
Apple Silicon). Read `docs/00-status.md` first for the global picture and
the track rules, then this file. Branch: `track/m9-tcg-aarch64`.

Where M8 (docs 13 and 16) attacked floating point — x87 and SSE on the
host FPU, the biggest single win so far — this track is data-first: a
profile of real XP workloads under TCG on the Air says what the rest of
the vCPU time is made of (generated code vs helpers vs softmmu vs
translation), and the optimization is chosen from that, not from a guess.
The candidates that were on the table when the track opened (2026-09-04,
morning discussion): keeping x86 flags live in NZCV inside the aarch64
backend (Box64/FEX style, plus FEAT_FlagM), eliding the per-access memory
barriers on a single vCPU, and the structural one — host page tables
mirroring the guest's so the softmmu TLB lookup disappears (only viable
inside a Hypervisor.framework VM, a multi-month project with an open
question about helper calls needing VM exits; not started).

## Scope and files (this track owns them)

- Tools: `tools/tcg-profile.sh` (the runner), `tools/tcg-profile.py` (the
  report), `tools/tcg-hot.py` (the second pass: host code of the hot
  guest instructions), `tools/tcg_profile_lib.py` (shared parser);
  `tools/hvf-el1/` (the Hypervisor.framework EL1 probe: a bare-metal
  Rust guest + host, `build.sh`, README there).
- QEMU patches: `patches/qemu/13-perfmap-darwin.patch` — `-perfmap` on
  every host (upstream gates the option and `tcg/perf.c` on Linux; the
  map writer is plain stdio); `14-jit-wx-state.patch` — the macOS JIT
  write-protect switch only when the state changes;
  `15-tb-invalidate-fast.patch` — TB invalidation on guest writes, four
  cuts (below); `16-tlb-floor.patch` — a 4096-entry floor for the dynamic
  softmmu TLB. Later patches of the track: 17–19.
- `tools/tcg-fps.py` (the guest's VGA frame rate from outside: distinct
  QMP screendumps per second, `FPS=` in the runner).
- Docs: this file, the M9 row of the tracks table in `docs/00-status.md`,
  the M9 section of `docs/08-roadmap.md`; a design doc (doc 18) once the
  optimization is chosen.
- Shared (rebase first, edit minimally, say so in the commit):
  `tools/qmpc.py` (the typer gained `~ \` ^ @ # $ [ ] { } ?`),
  `scripts/test.sh`, `patches/qemu/README.md`.

## How the profile works

`tools/tcg-profile.sh <image> <name> ['guest command']` boots the image
headless as a snapshot with `-perfmap` (`CDS=a.mds:b.iso` adds game discs
as ide-cd devices with CD audio through an AC97 card, the slots the player
uses; `QEMU_BIN=` another binary for an A/B, `QEMU_EXTRA=` more arguments,
`FPS=<s>` the frame-rate probe after the sample), types the command into
the Run dialog over QMP, lets it settle, then samples the *whole* QEMU process
with macOS `sample` at 1 ms for 30 s (Linux: `perf record`). The report
(`tcg-profile.py`) splits the vCPU thread's self time into generated code
/ helpers by family / softmmu slow path / translation + TB lookup /
interrupts / waiting / other, and maps every generated-code sample
through the perf map (one entry per translated guest instruction) to a
guest address → kernel / win32k / drivers / EXE / DLLs, hot pages, hot
instructions. `--hot` prints a `-dfilter` list of the hottest pages; a
second run with `DFILTER=` logs those TBs' guest bytes, optimized TCG ops
and host code, and `tcg-hot.py` weights them by the samples: host
instructions per guest instruction and their class mix, TCG op kinds
(memory accesses = TLB lookups, condition-code traffic, helper calls,
barriers), guest mnemonics, the hottest instructions decoded (capstone
from a uv venv; hex without it).

Gotchas met on the way (2026-09-04/05):

- `sample` shows no thread names; the vCPU thread is the one running
  `cpu_exec_loop`. It cannot unwind through generated code, so those
  samples are leaves under `cpu_tb_exec`.
- The XP images are Brazilian: Program Files is `C:\Arquiv~1`, and the
  keyboard layout is US-International, where `" ' ^ ~ \`` are dead keys —
  a typed `"` swallows the following space. Type 8.3 paths without
  quotes (`~` + digit yields both characters).
- Never edit a running bash script (the runner) — bash reads it
  incrementally and the run dies mid-way with a syntax error.
- QEMU's `-d out_asm` prints raw hex (`OBJD-H:`) in a build without
  capstone; `tcg-hot.py` classifies aarch64 words by opcode field instead.
- System-mode TCG emits the guest's memory-order barriers (`mb` before
  every load/store, `dmb ish` on aarch64) unconditionally — upstream keeps
  them even on one vCPU for the i/o threads' sake (`tcg_gen_mb`). The
  `QEMU_TCG_NO_MB=1` switch used for the barrier measurement was a hack in
  the tree, not a patch (prepare removed it).
- The second pass (`DFILTER=`) logs every translation of the filtered
  pages; on a workload that retranslates them 20k times a second (the
  game below) that is 6 GB in 30 s and the log writer takes over the
  vCPU thread. Turn it off over QMP (`log none`) or filter a cold page.
- The perf map spans every code-buffer epoch (a `tb_flush` restarts the
  bump allocator), so a host address names a different TB per epoch: the
  report tools now pick the epoch of the sample window from the `info
  jit` flush counts taken before and after the sample and print which;
  a window that straddles a flush is mis-mapped for its earlier part
  (keep `SECS` short on workloads that flush every minute).
- A guest's *frame rate* is the only honest before/after number for a
  game; `%` of a 100 %-busy vCPU says where the time goes, not how much
  work gets done. `tools/tcg-fps.py` counts distinct VGA screendumps per
  second (a game blitting a back buffer to the primary changes the VGA
  surface once per frame). Moto Racer's attract-mode intro reads 8 fps on both the
  baseline and the fixed build (2026-09-05) although the fixed build runs
  four times more guest code per second — the demo replays inputs at a
  fixed tick; the *race* moves (4.9 → 7.1–7.3 fps, `tools/xp-moto-race.sh`).
  Measure the game where the player plays it, and keep a fixed-work
  guest-reported oracle (7-Zip, Super PI) next to it.
- macOS JIT W^X (patch 14): a thread's initial write-protect state is not
  knowable — the main thread is in execute mode when `tcg_prologue_init`
  asks for write, a vCPU thread is not — so the tracked state must start
  as *unknown* and make the real call in either direction. A skipped
  first write call faults on the prologue store and the process spins at
  100 % inside `tcg_prologue_init` (QMP never answers: that is the
  symptom).

## State (2026-09-05)

Two things exist: the profiles below (patches 13 and 14 came out of
them), and the HVF EL1 probe (its own section further down, raw output
in `tools/hvf-el1/results-m1air-2026-09-05.txt`). Nothing of the
optimization itself is written yet; the next session starts at "Next
steps" with the user's pick.

All on the M1 Air (macOS 26.6.2), `-cpu pentium3`, one vCPU, XP SP3, 30–40 s
samples at 1 ms (`build/tcg-profile/<name>/report.txt`, `hot.txt`).

**Idle desktop.** The vCPU thread is 90 % in the halt wait; of the busy
10 %, translation + lookup 3.9 % (`tcg_flush_jmp_cache` — every CR3 write
flushes the 64 KiB jump cache — and the translator itself), 4 % QEMU
housekeeping, generated code 1.8 %.

**7-Zip 26.02 `b -mmt1`** (integer, LZMA; compress 1101 / decompress 1616
MIPS at dict 22, the "1T CPU freq" calibration reads ~3050 MHz): the vCPU
thread is 100 % busy — **generated code 77 %**, **translation + lookup
14 %** (13 % of it *self time in `helper_lookup_tb_ptr`*: every `ret` /
indirect jump leaves the TB to look the target up, ~70 host instructions
plus the register sync around the call), condition-code helpers 3.5 %
(`helper_cc_compute_c` for a carry read with the cc_op unknown at TB
entry), softmmu slow path 2.5 %, rest 2 %. 97 % of the generated-code
samples are in `7z.dll` (0x100d2000–0x100ee000); no kernel time to
speak of. Second pass (`hot.txt`, 98.5 % of the samples covered): per
guest instruction 23 host instructions, 6.7 TCG ops; 54 % of the hot
instructions carry a memory access (a softmmu TLB lookup each), 52 %
condition-code traffic, 4.5 % a helper call; guest mix `mov` 39 %, `jae`
22 %, `sub` 7.5 %, `jne` 6.6 %. **Where the samples land, by the exact
host instruction hit: 76 % on loads** — 43 % inside the TLB lookup
sequence (`ldp` mask/table → `ldr` tag + `ldr` addend → the access: a
chain of dependent loads per guest access), **41 % on guest registers
being loaded from / stored to `env`** (every TB starts by reloading the
registers it uses and every register write is stored back before the
next memory op, so a value flows store → load through memory at each
block boundary), 9 % other env traffic, 4 % arithmetic, 2 % the guest
access itself, 0.6 % block exits, 0.5 % barriers. With `sample` the PC is
the oldest unretired instruction, so this is a latency picture, not an
instruction count: the emulated CPU waits on load chains, and the chains
are the TLB lookup and the register file living in memory.

**7-Zip without the memory barriers** (`QEMU_TCG_NO_MB=1`, the `dmb`
before every guest load/store): compress 1098/1064 → 1145/1187 MIPS
(dict 22/23), decompress 1616/1460 → 1644/1483 — **+2 to +10 %, ~4 %
typical**, single runs. Correctness caveat before it becomes a patch:
device code that reads guest RAM outside the BQL (none known: the D3D
executor and the audio ring run on the vCPU thread or copy under the
lock).

**D3DGAME9 `-novsync`** (the reference Direct3D scene on the M4 device,
executor over KosmicKrisp): the vCPU thread is only 42 % busy — 58 % in
`__psynch_cvwait`, the guest waiting on the device / present path, so the
CPU is not the bottleneck of that scene here. Of the busy part: generated
code 22 %, translation + lookup 8.7 % (`helper_lookup_tb_ptr` 7 %), x87
helpers 3.9 % (`fldt`/`fstt` — 80-bit loads and stores stay helpers —
`fsin`, `fxam`, `fwait`, `fpop`), softmmu 1.9 %. 69 % of the generated
code samples in the EXE, 15 % in DLLs. Second pass (84 % covered): the
scene's hot code is x87 (`fstp` 16 %, `fld` 9 %, `fmul`/`fadd`/`fsub`/
`fdiv` 12 %, `fxam`/`fnstsw` 2 %), and under patch 06's shadow translator
an x87 instruction with a memory operand is 30–50 TCG ops and 50–80 host
instructions (`fld dword [esp+0x24]` 34 ops / 49 host; `fsub dword [mem]`
52 / 78): 77 % of the hot instructions carry a memory access, 11.5 % a
helper call (`fxam`, `fnstsw`, the 80-bit loads/stores); samples land
41 % on arithmetic ("work"), 35 % in the TLB lookup, 15 % on the shadow
state in `env`, 6 % on guest registers; a third of the samples are in
unchained TBs (the x87 mode exits). That is M8's "cheap-op throughput of
the shadow translator" item, seen from the host side; not this track's
first target.

**Super PI mod 1.5 XS, 1M** (x87 under patch 06; `C:\Docume~1\David\Desktop\SUPER_~1.5`,
driven with `KEYS='alt+c,down×6,ret,ret'`): vCPU 100 % busy — generated
code 72.5 %, **`pthread_jit_write_protect_np` 12 %** (macOS's per-thread
W^X switch for the MAP_JIT code buffer: `cpu_tb_exec` asks for execute
before *every* TB it runs from the main loop, and this loop returns to
the main loop constantly because patch 06's out-of-line x87 helpers —
`frndint`, `fistll`, `fldcw`, `fprem`, `fptan`, `fwait` — end their TB
with an exit; `cpu_exec_loop` + `cpu_tb_exec` themselves 3.2 %),
`helper_lookup_tb_ptr` 5.7 %, x87 helpers 2.6 %, softmmu 1.1 %; 86 % of
the generated code in the EXE (0x417000–0x42e000), 12 % kernel. The
redundant toggles are gone with **patch 14** (a per-thread state):
**1M in 1:36.2 → 1:25.3 (−11 %)**, same binary otherwise, same day, the
timed runs in `build/tcg-profile/superpi-before` / `-after`; the
underlying exits are M8's: an x87 helper exit could chain through
`lookup_and_goto_ptr` or not end the TB at all. Second pass (93 %
covered; samples: arithmetic 42 %, TLB lookup 32 %, shadow state in
`env` 18 %, guest registers 6 %): typical shadow-mode costs are
`fstp qword [mem]` 18 ops / 30 host instructions, `fld st(3)` 9 / 11,
`fadd qword [mem]` 34 / 55, `fxch st(3)` **44 / 48**, `fdivp` **88 /
117**, `fcomp qword [mem]` 50 / 64 — M8 follow-ups (`fxch` and `fdivp`
first). Beware in `hot.txt`: the last guest instruction of a TB carries
patch 06's slow blocks (thousands of host instructions), so per-
instruction averages there are meaningless; read the per-sample lines.

**FIFA 2000 on the M7 driver** crashed at start on this Mac on the
2026-09-05 morning image (XP's own error box); the user's updated
`winxp-m7.qcow2` (same day, evening) runs FIFA 2000 and Moto Racer 1997,
both slow, Moto Racer slowest — profiled below with the player's disc
layout (`CDS=MOTO_RACER.mds:FIFA2000.ISO VGA=d3dpt MEM=1024`, the game
started from the Run dialog as `cmd /k cd /d C:\Arquiv~1\MotoRacer &
MOTO.EXE`; FIFA is `C:\Arquiv~1\EASPOR~1\FIFA20~1\fifa2000.exe`, the
WineD3D DLLs already renamed away in the image).

### Moto Racer 1997, the in-engine intro (2026-09-05, evening): two pathologies, not "TCG is slow"

The intro is the game's own software renderer at 640×480×16 into a
system-memory back buffer blitted to the primary (`d3dpt_ddraw.log`:
plain DirectDrawCreate, no Direct3D). Baseline build (patches ≤ 14), 30 s
sample, vCPU 100 % busy: **generated code 14 %, softmmu slow path 49 %,
translation + TB maintenance 25 %**, perf-map writer 2 % (the profiler's
own cost). 97 % of the slow-path samples are `victim_tlb_hit` /
`mmu_lookup` / `do_ld4_mmu` / `do_st4_mmu` under the loads *and* stores of
one guest loop, with no page walks (`tlb_fill` 0.2 %); `info jit`: 600k TBs
translated and 557k invalidated in the 30 s. Two separate causes:

1. **The dynamic TLB was 64–256 entries.** `tlb_mmu_resize_locked` sizes
   the direct-mapped table at every flush from the entries used since the
   previous flush, and XP flushes at every context switch (450–750/s here),
   so the "working set" it sees is a few hundred pages and the 30–70 %
   use-rate policy settles at 64–256 entries (`CPU_TLB_DYN_MIN_BITS` is 6).
   At that load two live pages share an index all the time — a sampled
   victim hit every 65536th: `page 0x77bf1000 evicts 0x5f1000, 256
   entries` (both index 0xf1), `0x10dcc000 evicts 0xcd0c000, 64 entries` —
   and every access to either page is a victim-TLB swap (~40 ns, three
   entry copies under the TLB spinlock): **1.64 billion victim hits in a
   4-minute run, ~6 million a second**. A 4096-entry floor (**patch 16**,
   `CPU_TLB_DYN_MIN_BITS`/`DEFAULT_BITS` 12): 1.5 million hits in the same
   run (1000× fewer), slow path 49 % → 1.3 %, generated code 14 % → 57 %.
   128 KiB of entries per mmu index, a memset per flush (~0.5 % at XP's
   flush rate). Upstream's policy would want a conflict counter; the floor
   is the one-line version.
2. **TB invalidation storms, three sources.** (a) Physical page 0xca000 is
   QEMU's *vAPIC option ROM* (`kvm aPiC`, `hw/i386/vapic.c`): XP's HAL has
   its TPR accesses patched into `call`s to stubs there (`out 0x7e,al;
   movzx eax, byte [0x800ca300]; ret` per register, a `lock cmpxchg` on the
   same dword for raises) — every IRQL change of the guest runs them, and
   the `VAPICState` the APIC writes at every interrupt (`apic_sync_vapic`
   → `cpu_physical_memory_write`) is at 0xca300, *the same page*. The DMA
   side's `tb_invalidate_phys_range` rounds the first page's range down to
   the page start (a 2023 upstream regression, still in master
   2026-09-05), so every interrupt invalidated all 17 stub TBs, each
   invalidation flushing the whole 64 KiB jump cache (CF_PCREL), and they
   were retranslated ~5k times a second — *on every XP run, idle
   included* (idle: 136k TBs in 40 s; 7-Zip: 50k). (b) Writes to pages
   that carry any TB take the slow path and, before this patch, the page
   collection lock (two g_tree lookups) plus a walk of the page's TB list
   *per write* even when nothing overlaps: the vAPIC lock word (23k
   writes/s), pool pages with a stale TB (45k/s) — ~12 % of the vCPU. (c)
   **The game is genuinely self-modifying**: page 0x436000 of MOTO.EXE holds
   four unrolled copies of the textured-span inner loop (`add edx, imm32;
   adc bh, imm8; dec esi; mov [edi], ax; lea edi,[edi+2]; jmp`) whose
   immediates (texture pointer, u/v deltas, carry bytes, 52 bytes per
   round) are rewritten per span: 187k writes and 20.8k retranslations a
   second, values that really change (five page dumps 0.5 s apart differ
   at exactly those 52 bytes). **Patch 15** takes (a) away (range clamped
   to the write), makes (b) nearly free (a per-page byte range of its TBs:
   a write outside returns before any lock; a write that cannot hit a TB
   shared with a neighbouring page is handled under the page's own
   spinlock, no collection; the precise-SMC g_tree lookup of the writing
   TB only when a TB is hit) and drops the per-invalidation jump-cache
   flush (`CF_INVALID` already makes a stale slot miss). (c) remains:
   after both patches the vCPU is **generated code 57–59 %, translation +
   TB maintenance 25 %** (the list walk of the SMC page 9 %, the
   retranslations ~10 %, `helper_lookup_tb_ptr` 4 %), softmmu 1.3–1.9 %,
   `_tlv_get_addr` 3 % (macOS TLS: every `tcg_ctx` access in the
   translator is a call), perf-map writer 3 % (profiling only).

The remaining cost of (c) is the game's design meeting TCG's page-granular
code protection; ideas ranked: compare-before-invalidate in the store
slow path (skip when the patched bytes are unchanged — measured values do
change between spans, so probably small), a per-page 64-byte code bitmap
to shorten the walk (the patches land inside the code, so partial), and
the real one, "soft immediates": retranslate a TB that keeps being
invalidated with its patched immediates read from guest memory at
runtime and let writes to those bytes skip the invalidation (a translator
feature, days). Not before the register pinning of step 2.

**Correction (2026-09-05, later in the day): the generated-code time is
not the span loop.** The profiler attributed samples through a perf map
that merges every code-buffer epoch: TCG bump-allocates TBs and every
`tb_flush` restarts at the buffer's start, so a host address holds a
different TB in each epoch, and `load_map`'s "later entries win" picked
whichever entry started nearest — the game's hottest instruction came
out as `0x4365f6/ff/600` in one run, `0x46054e` in another, an ntdll
address in a third, a kernel one in the race. Fixed the same day
(`tcg_profile_lib.load_map(path, epoch)` splits the map where the
addresses jump back; `tcg-profile.py` / `tcg-hot.py` take the epoch from
the `TB flush count` of `info-jit-before/after.txt` and say so in the
report; a sample window that straddles a flush is flagged). Regenerated,
every Moto Racer run agrees: **80–90 % of the generated-code samples
(≈ 47 % of the vCPU) are one instruction, `rep movsd` at `0x46054e`** —
the game's own rectangle blit (`mov edi,eax; mov esi,ebp; ecx = width/4;
rep movsd; rep movsb; next row`, source and destination both system RAM:
a second of `memory_notdirty_write_access` traces shows no VRAM page).
The span loop of page `0x436000` is 2 % of the generated code; its
retranslation storm (25 % of the vCPU, the SMC pathology above) stands.
Second pass on the blit page (`moto-blit-d`, `hot.txt`): one `rep movsd`
iteration is **37 host instructions** — two `dmb` barriers, two softmmu
chains, and ESI/EDI/ECX loaded from and stored to `env` every iteration
with `cx_next` and the loaded value spilled to the stack, because the
loop's back edge is a TCG basic-block boundary and globals are synced
there; the samples sit on the two chains' addend loads and the loop-end
spill. The other runs with a flush (Super PI, D3DGAME9, FIFA) were
re-checked: their sample windows had no flush after them, so their
attribution was right.

`tools/hvf-el1` got the blit as kernels (`bench.S`, `exp_movs` /
`native_movs`, results in `results-movs-m1air-2026-09-05.txt`): rows of
640 bytes copied five ways, ns per dword, M1 Air, source and
destination of 32 KiB / 512 KiB / 8 MiB each (the numbers do not move
with the size):

| kernel | native | in the VM (env in the window) |
|---|---|---|
| today: TCG's loop transcribed from `qemu-d.log` (37 insns, registers through env, spills, barriers) | 2.11–2.16 | 4.4 (see the hazard below) |
| the same without the barriers | 2.12 | 4.2 |
| pinned guest registers + the softmmu chains (step 2 alone) | 1.17 | 1.17–1.19 |
| pinned registers + direct window accesses (the VM design) | 0.40 | 0.40 |
| a REP MOVS fast path: one softmmu probe of the source and one of the destination per run inside a page, then a 16-byte vector copy | 0.09 | 0.09 |
| libc memcpy per row (the floor) | 0.05 | — |

So on this loop the barriers cost nothing, pinned registers 1.8×, the
VM's mirror 5×, and a fast path 23× — without a VM. The fast path is a
translator change (patch 17 below): `rep movs`/`stos` with count ≥ N
probe both pages once per page-run (`probe_access`-style, refusing
MMIO / not-dirty / watchpoint pages and overlapping runs to the
per-element loop), copy with host memcpy, and restart the instruction
at the page boundary with ECX/ESI/EDI updated; the per-element loop
stays for the rest. Windows' own `memcpy`/`memset` (`RtlCopyMemory`,
`RtlFillMemory`) are `rep movsd`/`stosd`, so every guest benefits.

**A hazard the VM design must respect (found on the way).** In the VM
the pinned kernels first ran at 2.5 ns instead of 0.40 (`results-movs`,
the `env identity` lines): a store to `env` followed by a store through
the window costs ~2.2 ns extra per pair when `env` lives in the
payload's identity map (2 MiB blocks, global) and the window is 4 KiB
non-global pages — the `diag3` kernels isolate it (env store + window
load: fine; env store + window store: 2.5 ns; the same through the
identity view of the same RAM: fine; every single-mapping pattern,
stack included: fine). With `env` placed inside the window the loops run
at native speed; "today" stays at 4.4 because its stack spills still go
to the block-mapped image. Rule for the port: everything translated
code stores to — `env`, the TCG stack frame, the TLB, the TB cache —
must be mapped with the same kind of stage-1 entry as the guest window
(4 KiB pages; whether the global/non-global mix or the block/page mix is
the trigger is a one-hour follow-up in the probe). The verdict below
stands with that constraint added.

**The race, A/B** (`tools/xp-moto-race.sh`: demo → title → Play Solo →
Practice → Speed Bay, throttle held, `tools/tcg-fps.py` for 15 s;
`moto-race-base` / `moto-race-fix`): **4.9 → 7.1–7.3 fps** (+45–50 %, two runs of the fixed build). Less than
the four-fold rise of the generated-code share because the frame now
pays the SMC retranslations (25 %) and, above all, the blit's `rep
movsd` (the correction above: 90 % of the race's generated code too). Two leads beyond this track: the game's options screen says
`D3D: NOT DETECTED` — Moto Racer has a Direct3D 5 renderer and its HAL
probe fails against our M7 driver; with it detected the software
rasterizer (and all of the above) would be bypassed (an M7 item). And the
attract-mode intro reads 8 fps on *both* builds while the race moves, so
the demo is paced by the game (replayed inputs at a fixed tick).

**FIFA 2000 under the runner** (`fifa-fix`): `fifa2000.exe` started from
the Run dialog exits within 45 s, no error box on the screendumps, vCPU
idle. Under the player the user runs it fine, with the discs in another
slot order (the runner's `CDS` puts the guest-tools ISO on ide.1/0 and the
games on the slave slots, so FIFA's disc is F:, not the letter the
install recorded); `tools/xp-fifa2000.bat` / `tools/xp-fifa-match.sh` (M7)
know its start sequence. Not profiled yet — next steps.

**7-Zip A/B, same day** (`7zip-ab-base` / `7zip-ab-fix`, dict 22 / 23,
single runs): compress 1112 / 999 → 1137 / 1049 MIPS, decompress 1582 /
1504 → 1621 / 1547 (+2–5 %). Its working set never conflicted in the small
TLB (softmmu 1.4 % in both), so this is the vAPIC storm and the
jump-cache flushes alone; the games are where patch 16 bites.

Runs: `build/tcg-profile/moto-intro` (baseline), `-fix1` (range clamp),
`-fix2` (+ TLB floor), `-fix3` (+ jump cache), `-fix5` (+ page ranges,
single-page path), `moto-ab-base` / `moto-ab-fix` (A/B with the frame
probe), `moto-intro-d/hot-tbs.txt` (the vAPIC stubs' bytes),
`moto-intro-d/trace-1s.log` (one second of `translate_block` and
`memory_notdirty_write_access` events, taken over QMP with
`trace-event-set-state` on a kept guest).

**What the data says.** On integer code the cost is (1) the register
file in memory at block boundaries, (2) the four-load TLB chain per
guest access, (3) the out-of-line TB lookup for every return and
indirect jump, then (4) the carry helper at TB entry and (5) the
barriers. Flags computation itself is cheap (lazy cc_op, inline compare
+ branch); the x87/SSE work of M8 does not show up any more except the
80-bit `fld`/`fst` and the transcendental helpers on the D3D scene.
Structural fastmem (host page tables) is the only fix for (2); (1) and
(3) are translator work with ARM's register budget: the aarch64 backend
allocates from x20–x28 first (nine callee-saved registers, x19 = env),
enough to pin the eight GPRs plus `eip` for the life of a chained run
of TBs and store them only before helper calls, in the memory slow-path
stubs and in the epilogue.

**Where the Hypervisor.framework "fastmem" idea stands after the profile
(2026-09-05).** HVF cannot run x86 code on Apple Silicon; its only use is
to run TCG's Arm output inside an Arm VM whose stage-1 tables mirror the
guest's x86 page tables, so the software TLB lookup disappears. The data
says that is worth at most the TLB chain's share (43 % of generated-code
samples on 7-Zip, ~a third of the vCPU), and nothing of the other half
(registers through memory at block boundaries, the TB-lookup helper,
block exits). Against it: generated code calls C helpers constantly
(4.5 % of the hot instructions on 7-Zip, 11 % on the D3D scene, plus
every TLB miss and interrupt), and inside a VM each is an exit of about
a microsecond — more than the TLB lookup costs today — unless the
helpers, i.e. most of QEMU, run inside the VM at EL1 with no macOS
underneath. That was the open question; the probe below (same day)
measured every primitive of that design on the Air.

## The HVF EL1 probe (2026-09-05): what the VM design would cost

`tools/hvf-el1/` (README there) is a 37 KiB bare-metal Rust guest that
runs at EL1 under Hypervisor.framework with its own MMU, vectors and a
fault-driven mirror of an x86-style page table, plus the host that maps
a 64 MiB arena into it **at the same address in the host process, as IPA
and as guest VA** (pointers are shared, so `env`, the TB cache and guest
RAM could stay where QEMU has them), services its exits and runs the
same load kernels natively. M1 Air, macOS 26.6.2, two runs, the arena at
0x6_0000_0000 and 0xb_0000_0000; ~2 s. The hardware facts first: the VM
exposes a 4 KiB granule (`TGran4=0`; the x86 pages need it), 8-bit ASIDs,
36-bit IPA, no hardware A/D flag updates, `CTR_EL0.DIC/IDC=0` (cache
maintenance needed after JIT writes).

**Exits vs in-VM work** (the question the paragraph above left open):

| Primitive | ns |
|---|---|
| VM exit, `hvc` round trip (guest → host → guest) | 845–870 |
| VM exit, MMIO load (stage-2 miss → host emulates → retry) | 830 |
| in-VM exception (`svc` → EL1 vector → full frame incl. q0–q31 → `eret`) | 40 |
| in-VM helper call (`bl`) | 0.9 |
| host thread kick → `hv_vcpus_exit` → guest IRQ handler | 1.9–2.2 µs median (1.0–1.2 of it until `hv_vcpu_run` returns) |

An exit is ~900 helper calls or ~21 in-VM exceptions. On 7-Zip a
helper runs every ~20 ns of guest work (4.5 % of the hot instructions
at ~0.9 ns per guest instruction), so a design that exits for helpers
would be ~40× slower than today: **helpers must run inside the VM**,
which is settled. What the probe adds is that the runtime this needs is
small and works: exception vectors, page tables, a walker, an allocator,
`core::fmt` — the whole guest is 37 KiB and boots in a page of assembly.

**The memory model** (the x86 page tables mirrored in stage 1, one root
per CR3 with an ASID, pages mirrored read-only until the x86 D bit is
set — `tlb_set_page`'s rule):

| Event | ns |
|---|---|
| HVF populating stage 2 on the guest's first touch of a RAM page (once per page, in the kernel, no exit) | 745 / 4 KiB |
| first touch through the window: abort → x86 walk → PTE → `eret` (the softmmu refill) | 110–160 |
| hardware TLB miss with the tables warm (nested stage-1+2 walk) | 6–6.5 |
| x86 #PF delivered to the resume point (the in-VM `cpu_loop_exit`) | 67 |
| first write to a clean page: permission fault → D → remap RW → `tlbi` | ~495 (`vae1` non-broadcast is no cheaper) |
| CR3 switch, tables kept (`msr ttbr0_el1` + ASID) | 19 |
| the flush model instead (drop the mirror, refault 1024 pages) | 112 / page |
| JIT: patch + `dc cvau`/`ic ivau`/`isb` + call, in the VM | 134 (native, with the W^X toggle pair: 173) |

Correctness checks pass: A and D bits are set by the walk as x86 would,
a read-only page's write yields error code 3, an unmapped page's read
yields 0, two CR3s see their own pages through one window, code written
by the host process executes in the VM after cache maintenance.

**The loads**, ns per load, the kernels of `bench.S`: `direct` is one
`ldr w, [x_win, w_addr, uxtw]` (what translated code would emit with the
mirror), `softmmu` the exact `prepare_host_addr` sequence tcg/aarch64
emits today (`ldp` mask/table, index, comparator, addend, compare,
branch, load) against a TLB sized as QEMU's dynamic one and always
hitting, `pinned` the same with mask/table already in registers (next
step 3). "native" is the host process on macOS's 16 KiB pages — QEMU's
situation today; "VM" is 4 KiB guest pages under stage 2.

| set | chase VM direct | VM softmmu | native softmmu | native direct | indep VM direct | VM softmmu | native softmmu | native direct |
|---|---|---|---|---|---|---|---|---|
| 64 KiB | 1.24 | 3.22 | 3.23 | 1.25 | 0.31 | 0.64 | 0.64 | 0.31 |
| 4 MiB | 3.50 | 5.80 | 4.76 | 2.37 | 0.80 | 1.12 | 1.14 | 0.71 |
| 8 MiB | 5.55 | 10.4 | 10.2 | 5.10 | 0.89 | 1.37 | 1.45 | 0.86 |
| 16 MiB | 28.1 | 33.1 | 12.3 | 7.3 | 2.57 | 4.82 | 4.11 | 2.52 |
| 32 MiB | 29.0 | 35.1 | 14.3 | 7.7 | 3.27 | 6.02 | 5.44 | 2.81 |

Reading it: while the working set fits the L2 TLB (3072 entries → 12 MiB
at 4 KiB), the mirror removes the whole chain — a dependent guest load
costs 1.2–5.5 ns instead of 3.2–10.4, independent ones 0.3–0.9 instead
of 0.6–1.4: **1.4–2.6× per load, against native softmmu**, which is the
43 %-of-samples chain the profile found. `pinned` (the mask/table pair
in registers) buys nothing on dependent loads and 10–20 % on
independent ones, so next step 3 is worth less than hoped. Beyond
the TLB reach the picture inverts on random dependent access: a 4 KiB
page miss under stage 2 costs ~21 ns (16 and 32 MiB chase: 28–29 ns vs
native softmmu's 12–14 on 16 KiB pages), while independent loads still
win (2.6–3.3 vs 4.1–5.4) because the core overlaps the walks. This is
the one real performance risk of the design and it is measurable
before building anything: the vCPU's distinct-page working set per
second on 7-Zip / FIFA / D3DGAME9 (a counter in `tlb_fill`). Mitigations
exist — XP's large pages (PSE) become 2 MiB stage-1 blocks, contiguous
x86 physical runs can take the Contiguous hint (64 KiB entries) — but
they are workload-dependent.

**What "most of QEMU inside the VM" actually is** (inventory of the
tree, 2026-09-05). The code that must run in the VM is the vCPU core,
~45 KLOC of C that is already self-contained: `tcg/` (tcg.c 6.6 K, the
optimizer 3 K, the aarch64 backend 3.5 K, region.c), `accel/tcg/`
(cpu-exec, translate-all, translator, tb-maint: 3.5 K), `target/i386/tcg/`
(translate + decoder + emitter 11.8 K, the helpers 8 K incl. fpu 3.5 K
and seg 2.5 K, `sysemu/excp_helper.c` = `mmu_translate`), `fpu/softfloat.c`
5.3 K, `util/qht.c`. Its outside surface, by grep of those files:
glib = `g_malloc/g_free/g_new` (≈35 sites), `g_hash_table` (a handful,
tb-maint/region), `g_assert`; `qemu_log*` (~60); `qemu_mutex/spin` (page
and TB locks, ~30) and `bql_lock/unlock` (20: interrupt delivery and
device access — the host handshake); `rcu` reads (10); `sigsetjmp/
siglongjmp` (4); `mmap/mprotect` (region.c, the code buffer);
`qemu_thread_jit_write/execute` (the W^X toggles — gone); the memory
API only in the page walker (`address_space_ld*`, 6 sites → loads through
the identity map); `cpu_get_apic_*`, `cpu_get_pic_interrupt`,
`cpu_get_tsc`, `qemu_clock_get_ns` (device and clock access → the
mailbox, `cntvct` with a host-provided scale). The one deep change is
**`cputlb.c` (2.9 K) replaced by the fault-driven mirror** — this probe's
`mmu.rs` is 230 lines of the idea — which re-expresses every `TLB_*`
flag as a stage-1 permission: `TLB_NOTDIRTY` for pages holding TBs
(write-protect, fault → `tb_invalidate`), dirty logging for the
framebuffer (write-protect, fault → dirty bit), `TLB_MMIO` (not mapped
→ fault → exit to the device model, 0.83 µs — the same slow path as
today's `io_readx`, and qemu-3dfx's / d3dpt's FIFOs are RAM, only their
doorbells are MMIO). The ~70 `cpu_ld*/st*` sites in the helpers become
window accesses under the same fault rule. Devices, the memory API,
block, display, audio, QMP, the main loop stay in the player process;
the player binary carries the hypervisor entitlement (ad-hoc signed, as
QEMU's own build does).

**Verdict.** The "months, uncertain" of the paragraph above is now
"weeks, with one known risk": every primitive of the design works on
the Air and is cheap (the whole probe took a day), the in-VM runtime is
tiny, the shared-address arena lets the vCPU core and the device model
keep sharing QEMU's data structures by pointer, and the port is a
mechanical freestanding build of ~45 KLOC plus a rewrite of cputlb's
2.9 K and a mailbox protocol (I/O, interrupts = kick + a pending word,
TB invalidation for DMA, clocks). A first XP boot inside the VM is a
4–8 week effort for a focused session series; the gain ceiling on
TLB-resident code is the 33 % of the vCPU the profile attributes to the
chain (1.4–2.6× per load), and the CR3 switch cost (today: TLB flush +
the 64 KiB jump-cache memset + refills; 19 ns with ASIDs) comes on top —
XP idle spends 3.9 % there. The risk is the nested-TLB penalty on
random working sets beyond 12 MiB, to be measured on the real workloads
before deciding. Steps 1 and 2 of the list below (inline TB lookup,
pinned registers) are orthogonal and carry over into the VM unchanged;
step 3 (pinned mask/table) is not worth a patch by itself.

## Build / test loop

```sh
scripts/prepare-qemu.sh && scripts/configure-qemu.sh          # after any patch edit
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.dylib
tools/tcg-profile.sh ~/vms/winxp.qcow2 idle                          # ~2 min
CDROM=~/vms/bench.iso tools/tcg-profile.sh ~/vms/winxp.qcow2 7zip \
    'cmd /k C:\Arquiv~1\7-Zip\7z.exe b -mmt1 40'
CDROM=~/vms/FIFA2000.ISO VGA=d3dpt BOOT_WAIT=120 WARM=150 SECS=40 \
    tools/tcg-profile.sh ~/vms/winxp-m7.qcow2 fifa 'cmd /k cd /d C:\Arquiv~1\EASPOR~1\FIFA20~1 & ... & fifa2000.exe'
python3 tools/tcg-profile.py build/tcg-profile/7zip --hot            # the -dfilter line
DFILTER=... tools/tcg-profile.sh ... ; venv/bin/python tools/tcg-hot.py build/tcg-profile/7zip
tools/hvf-el1/build.sh && build/hvf-el1/hvf-el1 build/hvf-el1/payload.bin   # the HVF probe, ~2 s
scripts/test.sh all                                                  # before every commit
```

## Next steps, in order

Done on the way: patch 13 (`-perfmap` on Darwin), patch 14 (the
redundant macOS W^X toggles: Super PI 1M 1:36.2 → 1:25.3), and from the
Moto Racer profile patches 15 (TB invalidation: the vAPIC ROM page
storm, per-page code ranges, no jump-cache flush per TB) and 16 (the
4096-entry TLB floor) — the game's vCPU went from 14 % to 57 % generated
code; then the profiler's epoch fix and the blit finding (the correction
in the Moto Racer section), which puts a REP MOVS fast path first. The user
picks the next item (the track was opened profile-first); the
recommended order, each with M8's methodology (an on/off switch as the
oracle, both guest batteries identical, `scripts/test.sh all` green,
7-Zip / Super PI / D3DGAME9 profiled before and after with the tools
above):

0. **The games, measured where the player plays them** (a day): FIFA 2000
   headless with the player's disc order (a `CDS_SLOTS=` or the M7
   scripts' layout), Esc past the video, the match via
   `tools/xp-fifa-match.sh`'s clicks, `tools/tcg-fps.py` (blind to frames
   presented through the 3D device — use the executor's frame counter or
   `PLAYER_DUMP_OUT` cadence there); Moto Racer's race on `winxp` vs
   `winxp-m7`; Super PI before/after patches 15/16. And hand M7 the
   `D3D: NOT DETECTED` lead — Moto Racer's Direct3D 5 HAL probe against
   our driver would take the software rasterizer out of the picture.
0b. **REP MOVS / STOS fast path** (patch 17, a day): the probe's table
   above — 23× on the blit loop that is half of Moto Racer's vCPU, and
   Windows' `memcpy`/`memset` everywhere. In `do_gen_rep` (or a helper
   called from it) for `movs`/`stos` with a count worth it: probe the
   source and destination pages once per run inside a page (refuse
   MMIO, not-dirty, watchpoints, overlap, DF=1 → the existing loop),
   `memcpy`/`memset` on the host, advance ESI/EDI/ECX, re-enter the
   instruction at the page boundary; `-cpu …,rep-fast=off` as the
   oracle; `tools/xp-moto-race.sh` fps and 7-Zip / Super PI before and
   after; the DOS battery identical on/off.
1. **Inline the TB lookup for indirect branches** (`ret`, `call *`,
   `jmp *`): the jump-cache probe (hash of pc, compare pc / cs_base /
   flags / cflags, `goto_ptr`) emitted as TCG ops instead of
   `helper_lookup_tb_ptr`, falling back to the helper on a miss. Portable,
   contained (`tcg_gen_lookup_and_goto_ptr` + the i386 `cpu_get_tb_cpu_state`
   bits as TCG ops); the 13–14 % self time of the helper on 7-Zip and 7 %
   on D3DGAME9 is the ceiling. Patch 15.
2. **Pinned guest registers across chained TBs** (the big one, aarch64
   only): `eax`–`edi` and `eip` live in x20–x28 for as long as TBs chain;
   stored to `env` only before helper calls that may read them, in the
   memory slow-path stubs (before `blr` — a fault never returns), and in
   the epilogue; reloaded in the prologue and after helpers that may
   write them. Touches the liveness pass (no `sync` of pinned globals
   before `qemu_ld/st`), `tcg_reg_alloc_start/bb_end`, the call path,
   the aarch64 prologue/epilogue and slow-path stubs. Design doc 18
   first. The 41 % of generated-code samples on register traffic is the
   ceiling; expect a good part of it plus the `eip` load/add/store at
   every block end. Property `pinned-regs=off` as the oracle. Patch 16.
3. **Shorter TLB chain**: the per-mmu-index mask/table pair loaded once
   per TB (or pinned) instead of per access — one dependent load less
   per guest memory access. The probe's `pinned` kernels say this is
   worth 0 % on dependent loads and 10–20 % on independent ones; fold it
   into 2 if it comes for free, don't make it a patch of its own.
   - **The VM design's risk number** (can go first, it is a day): a
     counter of distinct guest pages touched per second and the
     `tlb_fill` rate on 7-Zip / FIFA 2000 / D3DGAME9 (a `-d` line or a
     QMP query, temporary), to predict the nested-TLB penalty the probe
     measured (~21 ns per 4 KiB miss beyond 12 MiB of random working
     set). With that number the "TCG inside an HVF VM" port (the probe
     section above: ~45 KLOC freestanding + cputlb rewritten as the
     fault-driven mirror + a mailbox protocol, 4–8 weeks) becomes a
     decision instead of a bet; design doc 18 either way.
4. **Carry at TB entry**: inline the ADC/SBB/shift cases of
   `gen_prepare_eflags_c`, or carry the static cc_op into the TB lookup
   key (the high half of `cs_base` is free in 32-bit mode) so a TB knows
   the flags state it is entered with. ~3.5 %.
5. **Barriers off on one vCPU** (~4 %): only after an audit of every
   reader of guest RAM outside the BQL; as a machine property, not an
   env var.
6. **Self-modifying rasterizers** (Moto Racer: 20k retranslations/s,
   ~25 % of its vCPU after patches 15/16): compare-before-invalidate in
   the store slow path (small, values change), a 64-byte code bitmap per
   page to shorten the list walk (9 %), or "soft immediates" — a TB
   invalidated N times is retranslated with its patched immediates read
   from guest memory and writes to those bytes skip the invalidation (a
   translator feature, days). Also `_tlv_get_addr` 3 %: macOS TLS for
   `tcg_ctx` in the translator (cache it in a local in the hot paths).
