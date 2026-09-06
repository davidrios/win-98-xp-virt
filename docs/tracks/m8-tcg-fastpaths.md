# Track: M8 — CPU fast paths in TCG (docs 13 and 16)

The handoff for a session that works on how fast the emulated CPU runs
floating-point code: the x87 shadow-double translator (patch 06, doc 13),
the SSE inline path (patch 11, doc 16), the TCG float opcodes they added
to both backends, and their tests and benchmarks. Read `docs/00-status.md`
first for the global picture and the track rules, then this file, then
docs 13 and 16. Branch: `track/m8-tcg-fp` was merged to `main`
2026-09-04 and **deleted 2026-09-06** (local, worktree and remote);
branch fresh off `main` for the next M8 item.

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
  overlap today; keep it that way. Numbering: 05–09 are the x87 patches
  and the upstream i386 backports (07–09 came in from `main` on
  2026-09-04 with the M8 merge), 10 is the embed API (meson + `embed/`,
  disjoint files), 11–19 continue the CPU work — the SSE and SIMD patches
  were 07/08 on the track branch until that merge; older commit messages
  and the Air's notes still say so.

## State (2026-09-04)

- **Patches 05 + 06 (x87):** merged on `main`. XP Super PI 1M on the Air
  1:57 vs the rig's 2:02; PC=53 and PC=24 inline; both x87 tests pass.
- **Patch 11 (SSE) and 12 (MMX/integer + permutes, `simd-fast`,
  `tbl_vec`):** on this branch, aarch64-authored. `sse-guest-test.py`
  546,425 lines identical `*-fast=on/off` on both the Air and this
  x86-64 box; `x87-guest-test.py` still passes with both on. Register-
  only bench on the Air: SSE packed 12×/scalar 3.6×, MMX chain 4.0×.
  `SSEBENCH.EXE` in XP on the Air: SSE kernels 3.2–7.4×, x87 10–12×
  (`reference/benchmarks/README.md`). **Rig row (real P4) done
  2026-09-04:** every `check` identical to the Air's; Air ÷ rig per
  kernel 97–109 % on four kernels, 61 % packed transform, 18 % x87 C
  transform — clamp+cmp's 34 % is now closed on x86-64 (below).
- **x86-64 backend, validated 2026-09-04** (this worktree, `.claude/
  worktrees/m8-tcg-fp`; first time this codegen ever ran — it shipped
  aarch64-only until now). `x87-guest-test.py` (382,251 lines) and
  `sse-guest-test.py` (546,425 lines) both bit-identical fast-path
  on/off throughout everything below; x87 8.1×/6.7×, packed SSE 7.5×,
  scalar SSE 3.9×. Four fixes landed this session, each with its own
  commit and `scripts/test.sh all` green:
  1. **`simd_psadbw` regression:** it used the memory-based
     `tcg_gen_gvec_umax/umin/sub` (built for large arbitrary-length
     vectors) instead of the register-only `umax_vec`/`umin_vec`/
     `sub_vec` `simd_tbl_permute` already used in the same file — net
     *slower* than the helper it replaced. Switched to the register-only
     ops; parity restored.
  2. **Native `pmulhw`/`pack*`:** x86-64 has no cheap bitfield-insert
     (aarch64's `BFI`/`SBFX`), so the portable SWAR fallback only hit
     3–3.9× there. Added two x86-64-only generic TCG vector opcodes —
     `mulsh_vec`/`muluh_vec` (`PMULHW`/`PMULHUW`) and `ssnarrow_vec`/
     `usnarrow_vec` (`PACKSSWB`/`PACKUSWB`/`PACKSSDW`) — mirroring how
     `tbl_vec` was added (`tcg-opc.h`/`tcg.c` declare, `tcg-op-vec.c`/
     `tcg-op-common.h` wrap, `tcg/i386/tcg-target.{h,c.inc}` implement,
     `tcg/aarch64/tcg-target.h` macros `0`, unvalidated SWAR fallback
     kept there). Gotcha: a 128-bit `PACKSSWB`'s low 64 result bits come
     from *all 8* words of its first operand, not a 4+4 split, so MMX's
     4-word operands must be concatenated into one 128-bit register
     before narrowing (zero-padding and narrowing separately silently
     drops the second operand). MMX chain 1.4× → 1.7×.
  3. **`packuswb`'s scratch round trip:** the 128-bit concatenation above
     went through `env->sses_scratch` — two GP stores immediately
     reloaded by a differently-sized vector load, a textbook
     store-to-load-forwarding stall (the same anti-pattern doc 16 names
     for the aarch64 shuffle path, just not recognised as the same thing
     until now). Replaced with `dup_i64_vec` (broadcast each half across
     a register) + `bitsel_vec` (blend against a constant lane mask,
     `SIMD_TBL_MASK64LO` in `env->simd_tbl`) — both stock portable TCG
     ops, no new opcodes needed. MMX chain 1.7× → **2.1×**.
  4. **clamp+cmp (was 34% of the rig):** `minps`/`maxps`/`cmpps` had no
     native TCG vector op (unlike `fadd_vec` etc.), so they were
     synthesized from a 16-18-op "total-order key" bit transform — fold
     −0/+0 to a shared key, flip the magnitude bits of negatives — purely
     to give the integer-only `cmp_vec` a monotonic float ordering. On
     x86-64 that's unnecessary: the host speaks the guest's own ISA, so
     two new opcodes — `fmin_vec`/`fmax_vec` (mirrors `fadd_vec`) and
     `fcmp_vec` (a `cmp_vec`-shaped op whose third argument is literally
     the guest's own CMPPS/CMPPD predicate 0-7, no `TCGCond` translation
     needed) — map straight onto native `VMINPS`/`VMAXPS`/`VCMPPS`
     (`have_avx1`-gated like `fp_vec`; aarch64 macros `0`). Native
     min/max/cmp already get -0/+0 tie-break and NaN-operand-selection
     right (same hardware as the guest), so the guard only needs a
     6-op NaN-presence check (`sses_vnan_ok`), not the 16-18-op key —
     `sses_vkeys`/`sses_vkey` become dead weight on this path and are
     skipped. `sse-guest-test.py`'s packed chain 8.1× → 10.0×; new
     isolated `SSEBENCHC` kernel (mirrors `ssebench.c`'s `k_clamp`)
     added to the same tool: **6.5×** on its own.

  Patches 11 and 12 were regenerated four times total across these
  fixes (recipe below); the last time both had to move together since
  12's context sits right where 11's clamp+cmp fix now inserts code.
  **aarch64 validated 2026-09-04 (the Air, macOS 26.6.2)** after the
  four fixes: the queue applies and builds clean, `sse-guest-test.py`
  546,425 and `x87-guest-test.py` 382,251 lines identical on/off, so
  the `0` macros do route every new opcode to the SWAR / key-transform
  fallbacks and the portable ops the guards now use (`bitsel_vec`,
  `dup_i64_vec`, `cmp_vec`) behave. Register-only bench on the Air:
  packed 13.8×, scalar 3.4×, MMX chain 3.6×, and the new `SSEBENCHC`
  clamp+cmp 8.7× on the key-transform path (the isolated kernel is
  cheap enough there that the x86-64 native-op win is specifically a
  clamp+cmp-vs-P4 story, not an aarch64 problem). Both guest checks
  PASS in `scripts/test.sh all`.
- **Merged to `main` 2026-09-04.** `main` had meanwhile grown three
  upstream backports in the 07–09 slots (x87 helper fixes, i386 decoder
  fixes, the 10.0 rep-string series) touching the same translator files,
  so the SSE and SIMD patches were re-sequenced after them as
  `11-sse-inline-tcg` / `12-simd-inline-tcg` (upstream fixes first, ours
  on top), regenerated with the recipe below (both applied over the
  backports with offsets only, no rejects, no source edits), forward-
  applied twice from pristine and byte-compared, `scripts/test.sh all`
  green on x86-64. Numbers, docs and tools say 11/12 from here on. **Air
  confirmed the same day:** full rebuild from the merged `main` (queue
  applies, QEMU + player build), `scripts/test.sh all` green — both
  batteries identical on/off (546,425 / 382,251 lines), register-only
  packed 13.8×, scalar 4.1×, MMX 3.7×, clamp+cmp 8.5×, x87 10.6×/5.7×;
  the native DXVK checks pass too. Nothing owed on aarch64.
- **XP-guest `SSEBENCH.EXE` on the x86-64 box, 2026-09-04** (Ryzen 7
  5700X, `-iter 20`, best of two passes; rows in
  `reference/benchmarks/README.md`): clamp+cmp **3.68 ns/op = 43 % of
  the rig** (Air 34 %; helper path on this box 11.33, so 3.1× — vs the
  register-only kernel's 6.5×). The XP loop is not register-only: per
  iteration it does an aligned 16-byte load and store (two softmmu TLB
  lookups) and a `movmskps`, which is still QEMU's stock helper call
  (`gen_MOVMSK` → `helper_movmskps_xmm`; patches 11/08 never touched it),
  so the native min/max/cmp are no longer where that loop's time goes.
  Everything else vs the rig on this box: normalize 89 %, scalar chain
  116 %, convert 130 %, MMX blend 122 %, C normalize 99 %, packed xform
  49 %, C xform 16 % (the shadow translator's cheap-op throughput, as on
  the Air). Every `check` identical to the rig's. Side fix: the script's
  QMP socket now lives in `/tmp` (the worktree path plus the config tag
  blew the AF_UNIX name limit on the third config).

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
including `TESTS/SSEBENCH.EXE`, is staged in `guest-tools/out/iso`, and
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
byte. The 2026-09-04 session did exactly this four times (three
single-patch, one compound — next paragraph). If the patch being
edited isn't the last one touching a given shared file (11 isn't: 12
edits the same handful of TCG files afterward), pull out every later
patch that shares a touched file too, or the later patch's own apply
breaks on the shifted context — it did for 12 when 11's clamp+cmp fix
landed, `git apply` gave no partial credit for hunks whose context
didn't move. Recovery is the same two-step diff, just also done for
the bumped patch: save its *payload* (the files it produces) before
removing it, restore the edited patch first and reapply the payload by
hand at the new position, then diff that hand-merged result against
the edited-patch-only tree to get the bumped patch's regenerated
hunks. Not needed when the edited patch is the queue's last toucher of
every file it owns (true for 12 itself, and for 11 on its first three
edits this session, before clamp+cmp added content 12 also touches).

Slow-path counters: `info registers` (QMP `human-monitor-command`) prints
`SSE-fast slow paths: guard= handover= helper= cvt/comis=`; a workload
whose `helper`/`cvt` counters grow by millions is living on the slow
path (an out-of-range `cvttss2si` loop cost 2× the helper before the
benchmark's convert kernel was fixed to stay in range).

## Next steps, in order

1. ~~**x86-64 host run + clamp+cmp.**~~ Done 2026-09-04 — see State
   above for the four fixes and ratios. ~~aarch64 validation~~ done the
   same day on the Air (State above): both batteries identical, suite
   green. ~~XP-guest `SSEBENCH.EXE` clamp+cmp on an x86-64 host~~ done
   the same day (State above): 43 % of the rig, up from the Air's 34 %,
   the rest of that loop being its load/store and the `movmskps` helper.
   ~~The branch is ready to merge to `main`~~ merged 2026-09-04 (State
   above; patches now 11/12). Cheap follow-ups if
   clamp+cmp is ever revisited: inline `movmskps`/`movmskpd` (a
   `cmp_vec`-free sign-bit gather, both hosts), then the per-operand TLB
   cost, which is item 3's memory-operand story. The x87
   C transform (18 % on the Air) is the cheap-op throughput of the
   shadow translator, a bigger redesign; note it, do not start it here.
2. **A real workload number.** A Direct3D title in XP (the M4 track's
   item) with and without both `*-fast=off`: the first end-to-end number
   for patches 06 + 11 together.
3. Cheaper packed checks: hoist the vector constants (two instructions
   each per packed op on aarch64); a vector-to-scalar move opcode would
   remove the `env->sses_scratch` round trip and let scalar ops use the
   vector shape too (doc 16 follow-ups).
4. Still on helpers: `cvtps2dq`/`cvttps2dq`/`cvtdq2ps`, the MMX-register
   conversions (`cvtpi2ps`/`cvtps2pi`), `pmuludq`, `haddps`/`addsubps`
   (SSE3), SSSE3, VEX forms (no era relevance). `shufps`/`unpck*` and the
   MMX set landed in patch 08.
