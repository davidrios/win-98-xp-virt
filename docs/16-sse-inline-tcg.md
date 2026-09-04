# 16. SSE on the host FPU: scalar and packed float inline in TCG (2026-09-04)

Doc 13 moved the x87 stack onto the host FPU. SSE was still every
instruction a helper call: `helper_addps_xmm` loops over four lanes of
`float32_add`, which (once the precision flag is sticky) is softfloat's
"hardfloat" path, a host multiply wrapped in classification checks,
inside a call frame with three pointer arguments. Direct3D-era game code
uses SSE for exactly the work that runs every frame: D3DX vector and
matrix routines are packed SSE, compiled scalar math and float-to-int
conversions are `ss` instructions. Patch `07-sse-inline-tcg` translates
the common cases inline. Files: `target/i386/tcg/sse-fast.c.inc`
(instruction logic, slow blocks, the packed vector path),
`sse-fast-lane.c.inc` (scalar lanes in general registers, included for
32 and 64 bits), TCG opcodes in `include/tcg/tcg-opc.h`, both backends.

## The mode: MXCSR admissible and PE already sticky

A TB is translated with `TB_FLAG_SSE_FAST` (bit 31) when

- `env->sse_fast_mode` is set: MXCSR has RC = nearest, FTZ and DAZ off,
  all six exceptions masked, and the CPU property `sse-fast` is on
  (default; `-cpu pentium3,sse-fast=off` is the control). Kept by
  `update_mxcsr_status()`, i.e. every `cpu_set_mxcsr` caller.
- the precision (inexact) flag is already set in `env->sse_status`.

The second condition is the trick that keeps the inline code flag-exact
without computing a residual per lane, as the x87 path has to: with the
inexact flag sticky, an admissible operation on the host can only raise
PE, which is invisible, so the only thing to verify is that nothing else
would have been raised. That is a classification of the result (or, for
compares, of the operands), one unsigned compare per lane. It is also
exactly the gate of softfloat's own hardfloat path (`can_use_fpu`), so
the helper path and this one agree on when the host FPU is acceptable.
In practice a thread's MXCSR gets PE within its first few float
operations and never loses it (Windows saves and restores MXCSR across
context switches with `fxsave`/`fxrstor`, flags included).

Consequences for the translator:

- `ldmxcsr`, `fxrstor` and `xrstor` end the TB (they can change MXCSR or
  clear the flags; the flag is a TB flag and a chained successor must
  not carry a stale one).
- A TB translated without the flag emits, after each helper that may
  raise PE, a check at the end of the instruction (`sses_helper_done`,
  emitted by the decoder after the register and flags write-back): if
  the fast mode applies now, exit to the next instruction. Without it a
  loop translated before its first inexact result would keep running
  the helpers through its direct TB chain.
- A runtime guard at the first inlined instruction of a fast TB
  (`sses_guard`: mode byte and flag word) re-executes from a correctly
  translated block if the state does not match. With the exits above it
  cannot fire; it costs four instructions per TB.

## Fast conditions

Softfloat with DAZ/FTZ off treats denormal operands as exact values and
raises nothing for them, so operands need no check. What must be
excluded is everything that would raise a flag other than PE, or where
QEMU's result differs from the host's (NaN payload rules). One table:

| Instructions | Fast when | Why that is enough |
|---|---|---|
| add, sub, sqrt (ps/pd/ss/sd) | result exponent field not all ones | no NaN operand, no infinity, no overflow; a tiny add/sub result is exact (both operands are multiples of the smallest denormal) and sqrt cannot underflow |
| mul, div, cvtsd2ss | result exponent field in [2, max−1], or an exact zero because a factor / the dividend is zero | excludes NaN, inf, overflow, division by zero and any underflow whether tininess is detected before or after rounding (a result that rounded up into field 1 is tiny before rounding) |
| rcp, rsqrt (ps/ss) | normal result | the helpers restore the flags, so only the value matters |
| min, max, cmp (8 predicates), comis, ucomis | no NaN operand | the compare is an integer compare of total-order keys (−0 folded to +0) when there's no native vector min/max/cmp (aarch64); on x86-64 (2026-09-04) `fmin_vec`/`fmax_vec`/`fcmp_vec` use the host's own `VMINPS`/`VMAXPS`/`VCMPPS` instead — same ISA as the guest, so −0/+0 tie-break and NaN-operand-selection are already correct, only the NaN-presence check remains. NaN would need QEMU's NaN selection and IE either way |
| cvt(t)ss2si, cvt(t)sd2si | \|x\| < 2^31 − 1/2 | the host conversion is exact for both roundings; softfloat returns 0x80000000 with IE otherwise |
| cvtsi2ss, cvtsi2sd | always | only PE |
| cvtss2sd | result not NaN/inf | an SNaN input raises IE |

Everything else takes the instruction's slow block: the unmodified helper
sequence out of line, then a TB exit to the next instruction, as
x87-shadow does; the slow blocks share its array and emitter. Nothing is
written before the check (all lanes are computed and checked, one
branch, then the stores), so the helper re-runs on intact operands.
Legacy SSE encodings only; VEX forms take the helpers. A TB with more
than 94 inlined x87 + SSE instructions falls back to helpers for the rest.

## Two code shapes

**Packed** (`ps`, `pd`): the vector unit. New TCG opcodes `fadd_vec`,
`fsub_vec`, `fmul_vec`, `fdiv_vec`, `fsqrt_vec` (vece MO_32/MO_64;
aarch64 `fadd.4s` etc., x86-64 `vaddps`/`vaddpd`), and the checks with
TCG's existing integer vector ops (`shli`, `cmp`, `and`, `sub`,
`andc`, `sari`, `bitsel`). TCG has no vector-to-scalar move, so the
lane mask goes through `env->sses_scratch` and two 64-bit loads feed
the branch. A packed `mulps` is ~25 host instructions on the M1
including constant materialization (was ~100 with four scalar lanes
in general registers, and ~130 for the helper).

**Scalar** (`ss`, `sd`, the conversions, comis): general-register lanes
with the new scalar opcodes `add_f32` .. `sqrt_f32` (i32 temps living in
vector registers, as doc 13's `add_f64` on i64), `cvt_i32_f32/f64`,
`cvt(t)_f32/f64_i32` (aarch64 `scvtf`, `fcvtns`, `fcvtzs`; x86-64
`vcvtsi2ss`, `vcvtss2si`, `vcvttss2si` and the sd forms). A scalar
`mulss` is ~20 host instructions; the vector shape was tried for
scalars and lost to the memory round trips it needs for lane 0.

Encodings were checked against clang's assembler (`otool`) on the Air;
the x86-64 backend additions are written to the same pattern as
patch 06's but have not been executed yet (no x86-64 host at hand on
2026-09-04): run `tools/sse-guest-test.py` on the Arch box first thing.

## Verification

`tools/sse-guest-test.py`: a DOS program enables SSE in real mode
(CR4.OSFXSR) and runs 61 instruction sequences (all of the table above,
memory-operand forms, cmpps with every predicate, a 4-op packed chain, a
7-op scalar chain with a conversion round trip, a mixed x87/SSE block
that exercises dirty x87 shadows across an SSE slow-block exit, a
120-instruction block that overflows the slow-block array, and the SSE2
double forms under `-cpu pentium3,+sse2`) over every pair of 47 single
and 33 double edge-case values (zeros, denormals, min/max, 2^31
boundaries, infinities, quiet and signalling NaNs), under MXCSR 1FA0
(inline mode), 1F80 (flags clear: the hand-over path) and 3FA0 (round
down: helpers). Every lane and MXCSR are printed. Result on the Air:
**333,875 result lines identical** with `sse-fast=on` and `off`.
`tools/x87-guest-test.py` still passes (382,251 lines identical, its
benches 7.9× / 4.3× as before).

The test found two bugs on the way: packed unary ops have no second
vector operand (assertion), and the hand-over exit after a helper ran
before the instruction's register write-back (`cvttss2si` lost its EAX);
that is why the check is now emitted by the decoder at the end of the
instruction.

## Performance

`SSEBENCH.COM` (in the same test run): register-only kernels, 40M
iterations, BIOS ticks, M1 Air:

| Kernel | helpers | inline | ratio |
|---|---|---|---|
| packed: mul, add, mul, add, div, max, min, sub (8 ops) | 4.56 s | 0.38 s | **12×** |
| scalar: mul, add, div, sqrt, add, comiss+branch, cvttss2si (7 ops) | 2.80 s | 0.77 s | **3.6×** |

That is ~1.2 ns per packed op inline (the loop is bound by the latency
of the dependent NEON chain, the checks overlap in the out-of-order
window) against ~14 ns for the helper, and ~2.7 ns per scalar op
against ~10 ns; the scalar path pays the general-register moves of its
checks. Memory-operand forms add the TLB lookup either way (a first
version of this kernel with memory operands showed 1.3× because those
dominated), so a real loop sees less than the ratio.

`guest-tools/src/ssebench.c` (`SSEBENCH.EXE` on the guest-tools ISO) is
the Win32 counterpart for the rig and the guests: D3DX-shaped kernels
(packed transform, packed normalize with `rsqrtps`, a scalar chain with
`comiss`, clamp + `cmpps`, `cvttss2si`/`cvtsi2ss`), the transform and
normalize again in plain C pinned to x87 at PC=53 (doc 13's path), and a
denormal-decay kernel that shows the slow-path cost; prints ns per op and
a mean "SSE score". `tools/xp-ssebench.sh` runs it in XP headlessly per
`-cpu` configuration and prints the slow-path counters (`info
registers`: guard, hand-over, helper, cvt/comis exits) around the run.

XP on the Air, 2026-09-04, best of two passes (ns per op): packed
transform 2.4 vs 17.7 with `sse-fast=off` (7.4×), packed normalize 2.1
vs 13.5 (6.3×), scalar chain 3.6 vs 11.8 (3.3×), clamp+cmp 3.7 vs 16.2
(4.4×), convert 2.3 vs 7.4 (3.2×); the x87 kernels 4.1 / 3.9 vs 45 / 47
with `x87-fast=off` (10–12×, unaffected by the SSE switch); the denormal
kernel 81 vs 49 (the slow path is 0.6× the helper). Counters over the
scored kernels: guard 0, hand-over 1, helper exits only from the
denormal kernel. Full table and the measurement pitfalls in
`reference/benchmarks/README.md`. The rig has not run it yet.

## Patch 12: the integer and permutation instructions (2026-09-04)

The transform kernel showed it: between the inlined `mulps`/`addps` of a
D3DX vertex transform sit four `shufps`, each a helper call; the MMX
loops of era blitters, mixers and codecs are all helper calls
(`punpck*`, `pack*`, `pmulhw`, `pmaddwd`, `pavg*`, `psadbw`, shifts by a
register count, `pshufw`), plus a helper call for the MMX entry
(`fpstt`/`fptags` reset) before every one of them. Patch 12
(`target/i386/tcg/simd-fast.c.inc`, property `simd-fast`) translates
those inline: the permutes (`shufps`, `shufpd`, all `unpck*`, `pshufw`)
through a new TCG vector opcode `tbl_vec`, a byte table lookup with
zeroing (`tbl` on aarch64, `vpshufb` on x86-64) whose index vectors are
precomputed per instruction and immediate in `env->simd_tbl` at CPU
init (a two-source permute is two lookups and an OR, one lookup when
both sources are the same register); on 64-bit halves in integer
registers the saturating packs (`smax`/`smin`/`umin` per lane),
`pmulhw`/`pmulhuw`/`pmaddwd`, the average identity
`(a | b) - ((a ^ b) >> 1)` lane-masked, `psadbw` as the vector unit's
byte `umax - umin` plus a SWAR sum, gvec scalar shifts with the
over-width count masked to zero (sign-filled for `psra*`); and three
stores for the MMX entry. Integer ops have no flags or modes, so
exactness is by construction; the on/off test covers MMX and XMM forms,
memory operands, self-operands, chains and a mixed x87/MMX sequence:
546,425 result lines identical. Register-only MMX chain (DOS bench):
1.98 s → 0.49 s (4.0×). Legacy encodings only (VEX forms, ymm,
`pmuludq`, SSSE3 stay on helpers; hosts without `tbl_vec` use a
bit-spreading / lane-store fallback that is kept in the file).

Two lessons from this patch. A first version wrote the shuffles as four
32-bit lane stores; the next instruction's 128-bit vector load of that
register then stalled on the M1 (no store-to-load forwarding across
several smaller stores), and the transform kernel got *slower* than with
the helper, whose stores were far enough away. Anything that feeds the
vector path must produce its result as one vector store: that is why
`tbl_vec` exists. And `decode->immediate` is sign-extended (0xB1 arrives
as −79): table indices must mask it, which the on/off test caught on the
first memory-operand form with a high immediate.

## Follow-ups

- `movlhps`/`movhlps`/`pshufd`/`pshuflw`/`pshufhw` were already inline;
  `pmuludq` and the SSSE3 set are not (no era relevance).
- The packed checks re-materialize their vector constants per
  instruction (two instructions each on aarch64); a constant pool or
  hoisting in the backend would trim ~6 of the ~25.
- `cvtps2dq`, `cvttps2dq`, `cvtdq2ps` (SSE2 packed conversions) and the
  MMX-register conversions still take helpers.
- Every guest memory access carries a `dmb` on the Air because the pc
  machine's `max_cpus` is above 1 (TCG emits TSO barriers when the TB
  is translated for parallel execution, `-smp 1,maxcpus=1` turns them
  off). Measured on the memory-operand bench: no difference, the M1
  makes them free; noted so nobody measures it again.
- The scalar path's checks move values between vector and general
  registers (`fmov`/`ins`/`umov`, ~4 cycles each on Apple cores); a
  vector-register formulation of the scalar checks that keeps lane 0 in
  place would need a vector-to-scalar move opcode in TCG.
