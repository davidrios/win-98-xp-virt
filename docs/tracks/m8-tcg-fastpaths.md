# Track: M8 — CPU fast paths in TCG (docs 13 and 16)

The handoff for a session that works on how fast the emulated CPU runs
floating-point code: the x87 shadow-double translator (patch 06, doc 13),
the SSE inline path (patch 07, doc 16), the TCG float opcodes they added
to both backends, and their tests and benchmarks. Read `docs/00-status.md`
first for the global picture and the track rules, then this file, then
docs 13 and 16. Branch: `track/m8-tcg-fp`.

## Scope and files (this track owns them)

- QEMU patches: `patches/qemu/05-x87-fast.patch`, `06-x87-inline-tcg.patch`,
  `07-sse-inline-tcg.patch`, `08-simd-inline-tcg.patch` (their rows in
  `patches/qemu/README.md`). Inside the tree: `target/i386/tcg/x87-fast.h`,
  `x87-shadow.c.inc`, `sse-fast.c.inc`, `sse-fast-lane.c.inc`,
  `simd-fast.c.inc`, the f32/f64 and vector float
  opcodes in `include/tcg/tcg-opc.h`, `tcg/tcg-op.c`, `tcg/tcg-op-vec.c`,
  `tcg/aarch64/`, `tcg/i386/`, and the hooks in `translate.c`,
  `emit.c.inc`, `decode-new.c.inc`, `fpu_helper.c`, `cpu.h`, `cpu.c`.
- Tests: `tools/x87-fast-test.c`, `tools/x87-guest-test.py`,
  `tools/x87-unwind-test.asm`, `tools/sse-guest-test.py` (both guest
  batteries run in `scripts/test.sh`'s guest stage).
- Benchmarks: `guest-tools/src/ssebench.c` (`SSEBENCH.EXE` on the ISO),
  `tools/xp-ssebench.sh` (runs it in XP headlessly per `-cpu` config, works
  on macOS: floppy image via mtools, no sfdisk), the DOS benches inside
  the two guest tests, the x87/SSE sections of `reference/benchmarks/README.md`.
- Docs: doc 13, doc 16, this file, the M8 row of the state table in
  `docs/00-status.md`.
- Shared (rebase first, edit minimally, say so in the commit):
  `scripts/test.sh`, `guest-tools/build-wrappers.sh`, `CLAUDE.md`,
  `docs/00-status.md` outside the M8 row. The QEMU tree files above are
  also touched by patch 40 (M4) only in `hw/`, so the queues do not
  overlap today; keep it that way (new CPU work goes into 05–09).

## State (2026-09-04)

- Patch 05 + 06 (x87): merged on `main`, XP Super PI 1M on the Air 1:57
  vs the rig's 2:02; PC=53 and PC=24 inline; both x87 tests pass.
- Patch 07 (SSE): on this branch. `tools/sse-guest-test.py` 333,875
  result lines identical `sse-fast=on/off` on the Air (single + double,
  three MXCSR settings, the hand-over case, a mixed x87/SSE block); the
  x87 test still passes with it. Register-only bench: packed 12×, scalar
  3.6× over the helpers (doc 16). The x86-64 backend additions are
  written to patch 06's pattern but **have never executed**.
- `SSEBENCH.EXE` in XP on the Air (2026-09-04, `tools/xp-ssebench.sh`):
  SSE kernels 3.2–7.4× with patch 07, x87 kernels 10–12× with patch 06,
  table in `reference/benchmarks/README.md`. **Rig row done 2026-09-04**
  (three runs, same mean; results uploaded over the LAN with
  `tools/upload-server.py`): every `check` value identical to the Air's;
  Air ÷ rig per kernel 97–109 % on scalar chain / convert / normalize /
  MMX blend, 61 % packed transform, **34 % clamp+cmp**, 18 % x87 C
  transform, 87 % x87 normalize; SSE score 3.17 vs 2.28 ns per op.
- Patch 08 (MMX / SSE integer + permutes inline, `simd-fast`, new TCG
  `tbl_vec` opcode): on this branch, guest test 546,425 lines identical
  on/off, MMX chain 4.0×; `MMX blend` kernel added to SSEBENCH (XP
  numbers in the table). The x86-64 `vpshufb` path is unexecuted like
  the rest of the x86-64 backend additions.
- **x86-64 host run, 2026-09-04** (Arch box, worktree
  `.claude/worktrees/m8-tcg-fp`): first execution of patches 07/08's
  x86-64 codegen. `x87-guest-test.py` (382,251 lines) and
  `sse-guest-test.py` (546,425 lines) both bit-identical fast-path
  on/off; x87 8.1×/6.7×, packed SSE 7.5×, scalar SSE 3.9×. The MMX
  register bench only hit 1.4× (below the script's 1.5× "fast path not
  active" warning), vs 4.0× on aarch64. Isolated the 8 ops with a
  throwaway microbench (each op alone, 300M iterations, BIOS-tick
  timed): found `simd_psadbw` was a genuine regression — **slower with
  `simd-fast=on` than the helper it replaces** on x86-64 (net ~17 ticks
  on vs ~11 off) — because it used the memory-based
  `tcg_gen_gvec_umax/umin/sub` (designed for large arbitrary-length
  vectors) instead of the register-only `tcg_gen_umax_vec`/`umin_vec`/
  `sub_vec` that `simd_tbl_permute` in the same file already uses.
  Fixed by switching `simd_psadbw` to the register-only vector ops
  (patch 08 regenerated per the recipe below, verified byte-identical
  after two `prepare-qemu.sh` runs); psadbw is now roughly at parity
  with the helper instead of behind it. Guest tests still bit-identical
  after the fix. The aggregate MMX chain ratio barely moved (still
  ~1.4×) because psadbw was never the dominant cost: `pmulhw` and
  `packuswb` are scalar SWAR (per-lane `sextract`/`deposit` loops) and
  only get 3–3.9× from removing the helper call — x86-64 lacks a cheap
  bitfield-insert equivalent to aarch64's `BFI`/`SBFX`, so the same
  portable code costs more host instructions here. Closing that gap for
  real needs native vector ops for multiply-high and saturating pack
  (mirroring how `tbl_vec` was added: a new generic opcode gated per
  backend), not attempted this session — x86-64 has native
  `PMULHW`/`PMULHUW`/`PACKSSWB`/`PACKUSWB` hardware for exactly this,
  aarch64 support would need separate design/validation.

## Build / test loop

```sh
scripts/prepare-qemu.sh && scripts/configure-qemu.sh          # after any patch edit
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.dylib  # .so on Linux
python3 tools/sse-guest-test.py     # must end "... identical", bench ratio printed
python3 tools/x87-guest-test.py     # the slow blocks are shared: run both
scripts/test.sh all                 # before every commit (policy)
tools/xp-ssebench.sh ~/vms/winxp.qcow2   # SSEBENCH.EXE in XP: default, sse-fast=off, both off
```

`guest-tools/build-wrappers.sh` on the Air stops at the M7 driver step
(`build-driver.sh`: no mingw DDK headers here); everything before it,
including `GAMEDIR/SSEBENCH.EXE`, is staged in `guest-tools/out/iso`, and
`xorriso -as mkisofs -o guest-tools/out/guest-tools-3dfx-<rev>.iso -V GUESTTOOLS -J -r guest-tools/out/iso`
makes the ISO (2026-09-04).

Editing a patch: edit the files in `qemu/`, copy them aside, move the
patch out of `patches/qemu/`, run `prepare-qemu.sh` (tree = the previous
patches), then `git -C qemu checkout --` every file that *only* this patch
touches (prepare restores only files some present patch lists; new files
of the patch may also linger and must not be in the "pre" diff),
regenerate the patch with `diff -u` against the copies with `--- a/` /
`+++ b/` headers (new files `--- /dev/null`), put it back, then
`prepare-qemu.sh` twice and compare the tree with the copies byte for
byte. The 2026-09-04 session did exactly this twice.

Slow-path counters: `info registers` (QMP `human-monitor-command`) prints
`SSE-fast slow paths: guard= handover= helper= cvt/comis=`; a workload
whose `helper`/`cvt` counters grow by millions is living on the slow
path (an out-of-range `cvttss2si` loop cost 2× the helper before the
benchmark's convert kernel was fixed to stay in range).

## Next steps, in order

1. ~~**x86-64 host run.**~~ Done 2026-09-04 (see State above): both
   guest batteries bit-identical, `simd_psadbw`'s gvec-vs-register-vec
   regression found and fixed. Still open before merging to `main`:
   `pmulhw`/`packuswb` real vectorization (native `PMULHW`/`PACKSSWB`/
   `PACKUSWB`, a new generic opcode gated per backend like `tbl_vec`) to
   close the x86-64 MMX chain gap for real (currently 1.4× vs aarch64's
   4.0×) — bigger, cross-backend, needs aarch64 validation too; not
   started this session.
2. **clamp+cmp at 34 % of the rig.** `minps`/`maxps`/`cmpps` cost 4.7 ns
   per op on the Air against 2.1 for `mulps`/`addps` in the same TB shape;
   find out why (the compare's mask materialisation? a per-lane check
   that the arithmetic ops skip? look at the TB with `-d op,out_asm`) —
   the one SSE kernel the rig still wins clearly. The x87 C transform
   (18 %) is the cheap-op throughput of the shadow translator, a bigger
   redesign; note it, do not start it here.
3. **A real workload number.** A Direct3D title in XP (the M4 track's
   item) with and without both `*-fast=off`: the first end-to-end number
   for patches 06 + 07 together.
4. Cheaper packed checks: hoist the vector constants (two instructions
   each per packed op on aarch64); a vector-to-scalar move opcode would
   remove the `env->sses_scratch` round trip and let scalar ops use the
   vector shape too (doc 16 follow-ups).
5. Still on helpers: `cvtps2dq`/`cvttps2dq`/`cvtdq2ps`, the MMX-register
   conversions (`cvtpi2ps`/`cvtps2pi`), `pmuludq`, `haddps`/`addsubps`
   (SSE3), SSSE3, VEX forms (no era relevance). `shufps`/`unpck*` and the
   MMX set landed in patch 08.
