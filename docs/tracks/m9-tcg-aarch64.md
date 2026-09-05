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
  guest instructions), `tools/tcg_profile_lib.py` (shared parser).
- QEMU patches: `patches/qemu/13-perfmap-darwin.patch` — `-perfmap` on
  every host (upstream gates the option and `tcg/perf.c` on Linux; the
  map writer is plain stdio); `14-jit-wx-state.patch` — the macOS JIT
  write-protect switch only when the state changes. Later patches of
  the track: 15–19.
- Docs: this file, the M9 row of the tracks table in `docs/00-status.md`,
  the M9 section of `docs/08-roadmap.md`; a design doc (doc 18) once the
  optimization is chosen.
- Shared (rebase first, edit minimally, say so in the commit):
  `tools/qmpc.py` (the typer gained `~ \` ^ @ # $ [ ] { } ?`),
  `scripts/test.sh`, `patches/qemu/README.md`.

## How the profile works

`tools/tcg-profile.sh <image> <name> ['guest command']` boots the image
headless as a snapshot with `-perfmap`, types the command into the Run
dialog over QMP, lets it settle, then samples the *whole* QEMU process
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
- macOS JIT W^X (patch 14): a thread's initial write-protect state is not
  knowable — the main thread is in execute mode when `tcg_prologue_init`
  asks for write, a vCPU thread is not — so the tracked state must start
  as *unknown* and make the real call in either direction. A skipped
  first write call faults on the prologue store and the process spins at
  100 % inside `tcg_prologue_init` (QMP never answers: that is the
  symptom).

## State (2026-09-05)

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

**FIFA 2000 on the M7 driver** crashes at start on this Mac (XP's own
error box, nothing in the device log — the M7 track's open "macOS run",
not a CPU matter); not profiled.

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
underneath (months, uncertain). Pinned registers can also hold the
per-mode TLB mask/table pair, shortening the chain by one dependent load
with no hypervisor. Revisit HVF only if, after items 1–3 above, the TLB
chain is still the ceiling.

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
scripts/test.sh all                                                  # before every commit
```

## Next steps, in order

Done on the way: patch 13 (`-perfmap` on Darwin) and patch 14 (the
redundant macOS W^X toggles: Super PI 1M 1:36.2 → 1:25.3). The user
picks the next item (the track was opened profile-first); the
recommended order, each with M8's methodology (an on/off switch as the
oracle, both guest batteries identical, `scripts/test.sh all` green,
7-Zip / Super PI / D3DGAME9 profiled before and after with the tools
above):

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
   per guest memory access.
4. **Carry at TB entry**: inline the ADC/SBB/shift cases of
   `gen_prepare_eflags_c`, or carry the static cc_op into the TB lookup
   key (the high half of `cs_base` is free in 32-bit mode) so a TB knows
   the flags state it is entered with. ~3.5 %.
5. **Barriers off on one vCPU** (~4 %): only after an audit of every
   reader of guest RAM outside the BQL; as a machine property, not an
   env var.
