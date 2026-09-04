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
  portable code costs more host instructions here.
- **x86-64 native mulh/pack, 2026-09-04** (same session): closed that
  gap for real with two new generic TCG vector opcodes, x86-64-only —
  `mulsh_vec`/`muluh_vec` (multiply-high, `TCG_TARGET_HAS_mulh_vec`,
  native `PMULHW`/`PMULHUW`) and `ssnarrow_vec`/`usnarrow_vec`
  (saturating narrow, `TCG_TARGET_HAS_pack_vec`, native
  `PACKSSWB`/`PACKUSWB`/`PACKSSDW`) — mirroring how `tbl_vec` was added
  (declared in `tcg-opc.h`/`tcg.c`, public `tcg_gen_*`/`tcg_*_supported`
  wrappers in `tcg-op-vec.c`/`tcg-op-common.h`, backend in
  `tcg/i386/tcg-target.{h,c.inc}`); `tcg/aarch64/tcg-target.h` defines
  both macros `0` (keeps the existing SWAR fallback there, unvalidated
  on that host — same caveat as the rest of the x86-64-only work here).
  `simd_mulh_vec`/`simd_pack_vec` in `simd-fast.c.inc` use them when
  `tcg_mulh_vec_supported()`/`tcg_narrow_vec_supported()`, falling back
  to the SWAR path otherwise. One real gotcha: a 128-bit
  `PACKSSWB`'s low 64 result bits come from *all 8* words of its first
  operand (not a 4+4 split), so naively zero-padding MMX's 4-word
  operands to 128 bits and packing them against each other silently
  drops the second operand entirely (caught by an isolated per-op
  microbench with hand-computed expected values, and a debug print of
  the actual emitted VEX bytes to confirm the encoding itself was
  correct — the bug was semantic, not an encoding bug). Fixed for MMX
  by building one combined 128-bit register (both operands concatenated
  via `env->sses_scratch`) and narrowing it against itself, keeping
  only the low 64 bits (`tcg_gen_stl_vec`). `mulsh_vec`/`muluh_vec` have
  no such issue (purely per-lane, safe to zero-pad). Guest tests still
  bit-identical (382,251 / 546,425 lines); x86-64 MMX register bench
  1.4× → **1.7×** (pmulhw net cost ~0, packuswb roughly unchanged —
  scratch-memory round trips still dominate there, a possible further
  optimization, not chased). Both patches regenerated per the recipe
  below, verified byte-identical after two `prepare-qemu.sh` runs.
- **packuswb's scratch round trip, 2026-09-04 (same session, continued):**
  chased the item just noted above. `env->sses_scratch` combine was two
  GP stores immediately reloaded by a differently-sized vector load — a
  store-to-load-forwarding stall, the same anti-pattern doc 16 already
  names for the aarch64 shuffle path (patch 08's "two lessons"), just not
  recognised as the same thing at the time. Replaced with `dup_i64_vec`
  (broadcast each 64-bit half across a whole register, register-only) and
  `bitsel_vec` (blend the two broadcasts against a constant lane mask,
  lane 0 all-ones / lane 1 all-zero) so the combine costs one memory read
  (the mask, `SIMD_TBL_MASK64LO` in `env->simd_tbl`, shared by every pack
  instruction) instead of two round-tripped stores plus a reload. Both
  ops are stock TCG vector infrastructure with portable SW fallbacks
  (`bitsel_vec` needs no `tcg_*_supported()` gate, unlike this session's
  earlier `mulsh_vec`/`ssnarrow_vec` additions — no new opcodes this
  time). x86-64 MMX register bench 1.7× → **2.1×**; guest test still
  546,425 lines bit-identical. Patch regenerated per the recipe below
  (only `cpu.h` and the new `simd-fast.c.inc` changed), verified
  byte-identical after two `prepare-qemu.sh` runs. `scripts/test.sh all`
  green (4 passed, the d3d/guest-ISO checks skip in this worktree as
  before, unrelated to this change).
- **clamp+cmp, 2026-09-04 (continued):** item 2 of the next steps below,
  chased on this host. `minps`/`maxps`/`cmpps` had no native TCG vector
  op (unlike `fadd_vec` etc, patch 07), so they were synthesized from a
  16-18-op "total-order key" transform (fold −0/+0 to a common key, flip
  the magnitude bits of negative values, feed the result to the
  integer-only `cmp_vec`) purely to give `cmp_vec` a monotonic float
  ordering — that transform, not a mask-materialisation cost or a
  per-lane check the arithmetic ops skip (the two things the item
  suspected), is what made clamp+cmp cost 5-6× add/mul's op count. On
  x86-64 it's unnecessary: the host already speaks the guest's ISA, so
  two new generic TCG vector opcodes — `fmin_vec`/`fmax_vec` (plain
  3-operand, mirrors `fadd_vec`) and `fcmp_vec` (a `cmp_vec`-shaped op
  whose third argument is literally the guest's own CMPPS/CMPPD
  predicate immediate 0-7, no `TCGCond` translation needed) — map
  straight onto native `VMINPS`/`VMAXPS`/`VCMPPS`
  (`TCG_TARGET_HAS_fp_minmax_vec`/`_fp_cmp_vec`, `have_avx1`-gated like
  `fp_vec`; aarch64 macros `0`, keeps the key-transform path there
  unvalidated, same caveat as this whole x86-64-only thread). Native
  min/max/cmp already implement the guest's -0/+0 tie-break and
  NaN-operand-selection exactly (bit-for-bit the same hardware), so the
  guard only needs a NaN-presence check (`sses_vnan_ok`, 6 ops: two
  `shli_vec` + two `cmp_vec` GTU against `nan_limit` + `or` + `not`) to
  decide the slow-path branch, not the key computation — `sses_vkeys`'s
  4-op-per-operand `sses_vkey` (`andc`/`sari`/`shri`/`xor`) is dead
  weight here and skipped entirely on this path. `tools/sse-guest-test.py`
  packed chain (already exercises `minps`/`maxps` inline) 8.1× → 10.0×;
  a new isolated kernel added to the same tool, `SSEBENCHC`
  (`maxps`/`minps`/`mulps`×2/`cmpltps`/`movmskps`, mirroring
  `ssebench.c`'s `k_clamp`), measures clamp+cmp on its own: **6.5×**
  over the helper. Guest test still 546,425 lines bit-identical (the
  packed chain already covered min/max; cmpps forms are in the integer
  battery). `scripts/test.sh all` green. Patches 07 (the new opcodes +
  `sse-fast.c.inc`) and 08 (its `tbl_vec`/mulh/pack additions share the
  same TCG files, rebased past 07's new content, same payload)
  regenerated per the recipe below — this time both had to move
  together, since 08's context sits right where 07 now inserts code;
  verified byte-identical after two `prepare-qemu.sh` runs each. aarch64
  side of both patches (07's key-transform fallback, 08's SWAR path)
  unvalidated as before, needs the Air.

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
byte. The 2026-09-04 session did exactly this three times. If the patch
being edited isn't the last one touching a given shared file (07 isn't:
08 edits the same handful of TCG files afterward), pull out every
later patch that shares a touched file too, or the later patch's own
apply breaks on the shifted context — it did here, `git apply` gave no
partial credit for hunks whose context didn't move. Recovery is the
same two-step diff, just also done for the bumped patch: save its
*payload* (the files it produces) before removing it, restore the
edited patch first and reapply the payload by hand at the new
position, then diff that hand-merged result against the edited-patch-only
tree to get the bumped patch's regenerated hunks. Not needed when the
edited patch is the queue's last toucher of every file it owns (true
for 08, hence "did this three times" not four).

Slow-path counters: `info registers` (QMP `human-monitor-command`) prints
`SSE-fast slow paths: guard= handover= helper= cvt/comis=`; a workload
whose `helper`/`cvt` counters grow by millions is living on the slow
path (an out-of-range `cvttss2si` loop cost 2× the helper before the
benchmark's convert kernel was fixed to stay in range).

## Next steps, in order

1. ~~**x86-64 host run.**~~ Done 2026-09-04 (see State above): both
   guest batteries bit-identical, `simd_psadbw`'s gvec-vs-register-vec
   regression found and fixed, `pmulhw`/`packsswb`/`packuswb`/`packssdw`
   given real x86-64 vector ops (`mulsh_vec`/`muluh_vec`,
   `ssnarrow_vec`/`usnarrow_vec`) — MMX chain 1.4× → 1.7×. ~~packuswb's
   scratch-memory round trip~~ Done 2026-09-04 (see State above):
   `dup_i64_vec` + `bitsel_vec` replace the `env->sses_scratch` combine —
   MMX chain 1.7× → 2.1×. Still open before merging to `main`: **aarch64
   validation** of this session's work (the new opcodes — `mulsh_vec`/
   `muluh_vec`/`ssnarrow_vec`/`usnarrow_vec` — are x86-64-only,
   `TCG_TARGET_HAS_*` macros `0` on aarch64 so the SWAR fallback should
   still be exercised there unchanged, but `bitsel_vec`/`dup_i64_vec` are
   portable stock ops so the aarch64 pack path changed too and needs the
   same check; needs the Air, not started here).
2. ~~**clamp+cmp at 34 % of the rig.**~~ Done 2026-09-04 on x86-64 (see
   State above): the cost was neither mask materialisation nor a
   per-lane check the arithmetic ops skip, but `sses_vkeys`'s
   total-order key transform, needed only because `cmp_vec` has no
   float form — new `fmin_vec`/`fmax_vec`/`fcmp_vec` opcodes give
   x86-64 the native instruction instead, `SSEBENCHC` 6.5× over the
   helper. **Still open:** an XP-guest `SSEBENCH.EXE` run on this host
   to get a number directly comparable to the rig's original 34 %
   (needs `guest-tools/build-wrappers.sh` + `tools/xp-ssebench.sh
   ~/vms/winxp.qcow2`, not run this session — no ISO built in this
   worktree); aarch64 validation (see item 1). The x87 C transform
   (18 % on the Air) is the cheap-op throughput of the shadow
   translator, a bigger redesign; note it, do not start it here.
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
