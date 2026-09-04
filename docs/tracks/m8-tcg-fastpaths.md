# Track: M8 — CPU fast paths in TCG (docs 13 and 16)

The handoff for a session that works on how fast the emulated CPU runs
floating-point code: the x87 shadow-double translator (patch 06, doc 13),
the SSE inline path (patch 07, doc 16), the TCG float opcodes they added
to both backends, and their tests and benchmarks. Read `docs/00-status.md`
first for the global picture and the track rules, then this file, then
docs 13 and 16. Branch: `track/m8-tcg-fp`.

## Scope and files (this track owns them)

- QEMU patches: `patches/qemu/05-x87-fast.patch`, `06-x87-inline-tcg.patch`,
  `07-sse-inline-tcg.patch` (their rows in `patches/qemu/README.md`).
  Inside the tree: `target/i386/tcg/x87-fast.h`, `x87-shadow.c.inc`,
  `sse-fast.c.inc`, `sse-fast-lane.c.inc`, the f32/f64 and vector float
  opcodes in `include/tcg/tcg-opc.h`, `tcg/tcg-op.c`, `tcg/tcg-op-vec.c`,
  `tcg/aarch64/`, `tcg/i386/`, and the hooks in `translate.c`,
  `emit.c.inc`, `decode-new.c.inc`, `fpu_helper.c`, `cpu.h`, `cpu.c`.
- Tests: `tools/x87-fast-test.c`, `tools/x87-guest-test.py`,
  `tools/x87-unwind-test.asm`, `tools/sse-guest-test.py` (both guest
  batteries run in `scripts/test.sh`'s guest stage).
- Benchmarks: `guest-tools/src/ssebench.c` (`SSEBENCH.EXE` on the ISO),
  the DOS benches inside the two guest tests, the x87/SSE sections of
  `reference/benchmarks/README.md`.
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
- Not measured yet: `SSEBENCH.EXE` anywhere (rig off, ISO not rebuilt).

## Build / test loop

```sh
scripts/prepare-qemu.sh && scripts/configure-qemu.sh          # after any patch edit
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.dylib  # .so on Linux
python3 tools/sse-guest-test.py     # must end "... identical", bench ratio printed
python3 tools/x87-guest-test.py     # the slow blocks are shared: run both
scripts/test.sh all                 # before every commit (policy)
```

Editing a patch: edit the files in `qemu/`, copy them aside, run
`prepare-qemu.sh` (it stops at the broken patch, which leaves the tree at
the previous patch = the "pre" state), regenerate the patch with `diff -u`
against the copies with `--- a/` / `+++ b/` headers (new files
`--- /dev/null`), then `prepare-qemu.sh` twice more and compare the tree
with the copies. The 2026-09-04 session did exactly this; the recipe is
also in the patch README.

## Next steps, in order

1. **x86-64 host run.** On the Arch box: prepare, build, then
   `tools/sse-guest-test.py` and `tools/x87-guest-test.py`. Anything
   wrong will be in `tcg/i386/tcg-target.c.inc` (VEX `vaddss`.. `vcvtsi2ss`
   / `vcvttss2si` encodings, `vaddps`/`vaddpd` via `gen_simd`) or in
   constraints (`C_O1_I1(r, x)` added). Then merge to `main`.
2. **SSEBENCH on the rig and the guests.** Rebuild the guest-tools ISO
   (`guest-tools/build-wrappers.sh`), run `SSEBENCH.EXE` on the P4 rig
   and in XP on the Air with defaults, `sse-fast=off`, and
   `sse-fast=off,x87-fast=off`; fill the table in
   `reference/benchmarks/README.md`.
3. **A real workload number.** A Direct3D title in XP (the M4 track's
   item) with and without both `*-fast=off`: the first end-to-end number
   for patches 06 + 07 together.
4. Cheaper packed checks: hoist the vector constants (two instructions
   each per packed op on aarch64); a vector-to-scalar move opcode would
   remove the `env->sses_scratch` round trip and let scalar ops use the
   vector shape too (doc 16 follow-ups).
5. Still on helpers: `cvtps2dq`/`cvttps2dq`/`cvtdq2ps`, the MMX-register
   conversions, `haddps`/`addsubps` (SSE3), VEX forms (no era relevance).
