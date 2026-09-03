# 13. x87 inline TCG fast path (exploration, 2026-09-02)

Question: patch 05 (doc 00, `patches/qemu/README.md`) moved the common x87
case onto the host FPU but still pays a helper call per instruction. How
much of the remaining gap closes if the float ops are generated inline in
TCG instead, and what does that take? This doc records the prototype on
branch `worktree-x87-inline-tcg` (patch `06-x87-inline-tcg`).

## Where the time went with patch 05

Profile of the 7-op DOS loop (`tools/x87-guest-test.py` bench: fld m64,
fmul m64, fadd m64, fstp m64, fld m64, fdiv m64, fistp m32), x86-64 host:

| Symbol | Share |
|---|---|
| `x87_binop` (the host-FPU body incl. checks and TwoSum) | 33 % |
| `merge_exception_flags` | 11 % |
| individual `helper_f*` (call frame, `save_exception_flags`, FT0/ST0 round trip) | 45 % |
| translated code (TLB lookups, FIP/FDP stores, calls) | ~2 % |

98 % in helpers. Forcing the body and the flag merge to inline into the
helpers (`always_inline`) only gave 10 %: the cost is the structure, not
the body. Every x87 op is one or two calls, the value passes through
`env->ft0` and `env->fpregs[]` as 80-bit floatx80, and each helper saves
and merges `fp_status` flags.

## What "inline in TCG" needed

TCG had no float opcodes, but it already has everything around them: the
register allocator handles vector registers (used by gvec), `tcg_out_mov`
in both backends moves i64 values between general and vector registers,
`tcg_out_ld/st` accept an i64 temp living in a vector register, and x86's
`tcg_out_movi` already handles it. So a scalar float op is an ordinary
opcode on i64 temps whose operand constraint is the vector class:

- `include/tcg/tcg-opc.h`: `add_f64`, `sub_f64`, `mul_f64`, `div_f64`,
  `fmsub_f64` (a·b − c, fused), gated by `TCG_TARGET_HAS_f64`.
- x86-64 backend: `vaddsd/vsubsd/vmulsd/vdivsd/vfmsub213sd`, constraints
  `C_O1_I2(x, x, x)` / `C_O1_I3(x, 0, x, x)`; requires AVX + FMA3 (new
  `CPUINFO_FMA`).
- aarch64 backend: `fadd/fsub/fmul/fdiv/fnmsub` (scalar D), constraints
  `C_O1_I2(w, w, w)` / `C_O1_I3(w, w, w, w)`; plus `tcg_out_movi` into a
  V register via `tcg_out_dupi_vec`. Encodings checked against llvm-mc;
  **not yet executed** (needs the M1 Air).
- TCI and other hosts: `TCG_TARGET_HAS_f64` is 0, the translator keeps
  emitting the helper calls.

About 200 lines of TCG core/backend change. The larger part is the i386
translator (`target/i386/tcg/x87-inline.c.inc`, ~450 lines): for each
inlined instruction it loads the operands from `env->fpregs[]`, runs the
same admissibility checks as `x87f_binop()` in x87-fast.h (exact double,
exponent window ±2^900, result normal, no underflow, divisor nonzero),
does the op, recovers the inexact flag from the TwoSum / FMA residual and
ORs `PE` into `fpus`, converts back and stores. All checks are branchless
(`setcond`/`movcond`) so the fast path has no labels: values are EBB temps
that never get synced to the stack at a branch. Any failed check branches
to the unmodified helper sequence before any state is written, so the
result is by construction identical to the helper path (which is
identical to softfloat, doc 00 / patch 05).

Gate: `env->x87_fast_mode` (1 byte, derived from `fpuc` by
`update_fp_status()`: PC=53, RC=nearest, PE masked, property `x87-fast`
on). One load + compare per instruction; `fldcw` changes it through the
existing helper.

Inlined: `fld/fst/fstp m64`, `fadd/fsub/fsubr/fmul/fdiv/fdivr` with m64 and
with `st(i)` in both directions incl. the popping forms, `fld st(i)`,
`fxch`, `fst/fstp st(i)`. Still helpers: `fist*`, `fcom*`, m32/m16/int
operands, transcendentals, PC=24 (Direct3D) mode.

## Results (x86-64, Ryzen 7 5700X; ns per x87 op in the loop above)

| Configuration | ns/op | vs softfloat |
|---|---|---|
| softfloat (`x87-fast=off`) | 22.8 | 1.0× |
| patch 05 (helpers on the host FPU) | 10.6 | 2.2× |
| patch 05 + `always_inline` of the helper body | 9.5 | 2.4× |
| **patch 06 (inline TCG)** | **6.9** | **3.3×** |
| 06 without FIP/FDP stores (experiment, not kept) | 6.4 | 3.6× |
| 06 without inexact-flag recovery (experiment, not kept) | 6.2 | 3.7× |

Correctness: `tools/x87-guest-test.py`, extended to every inlined form and
a PE-unmasked control word, 298,460 result lines identical on/off.

With the inline path 85 % of the time is in translated code and the one
remaining helper in the loop (`fistp`) is 12 %. The 7-op TB grew from
1208 to 3720 bytes of host code (~130 host instructions per x87 op).

## Why it stops at ~7 ns, and what the next level is

Two things bound the inline design, and the toggles above show the
bookkeeping (FIP/FDP, PE) is not one of them:

1. **Instruction count.** Each x87 op still converts floatx80 → double
   (window and exactness checks, ~20 integer ops), crosses to the FP
   domain and back, converts the result, and does the TLB lookup for a
   memory operand. ~130 host instructions per op at ~4 IPC is ~7 ns.
2. **The serial chain through memory.** Every result is written to
   `env->fpregs[]` as x80 and reloaded by the next instruction, so a
   dependent chain pays store-forwarding plus both conversions per op
   (~30 cycles). The DOS loop is such a chain; real x87 code has more ILP
   but the instruction count still caps it.

Both disappear only if the translator keeps x87 values as doubles across
instructions: track the stack top statically (3 bits of `fpstt` in the TB
flags, `fldcw`/`fninit`/`fldenv`/`frstor` end the TB), keep a per-slot
"exact double" shadow (`st_d[8]` TCG globals plus a valid mask) and only
materialize the x80 form at TB exit and before any helper. Then
`fld m64; fmul m64; fadd m64; fstp m64` is four host FP instructions plus
the residuals off the critical path, i.e. ~1–2 ns per op. That is the
"much larger project": a lazy-state JIT for the x87 stack with correct
materialization on every exit path (exceptions, helpers, `fxsave`, TB
end) and a TB-flag change that touches the i386 translator core; estimate
1500–2500 lines and a long tail of validation. Not started.

## Cheaper follow-ups inside the current design

- Inline `fist/fistp` (the remaining helper in FP loops): needs an
  f64 → int conversion; doable with `add_f64`/`sub_f64` (the 2^52 trick)
  and integer extraction, or a `cvt_f64_i64` opcode (`cvtsd2si`/`fcvtns`).
- PC=24 (Direct3D) mode: same code with binary32 opcodes.
- Drop the per-op FIP/FDP stores in favour of one store per TB (7 %);
  needs care around `fnstenv` after a mid-TB exception.
- Keep the double in a vector register across a dependent pair by having
  the x80 store forwarded by TCG (no: TCG does no memory forwarding, this
  is the level-3 design again).

## How to test on the Air

```sh
git fetch && git checkout worktree-x87-inline-tcg
scripts/prepare-qemu.sh && scripts/configure-qemu.sh
ninja -C build/qemu qemu-system-i386 libqemu-embed-i386.dylib && cargo build --release
python3 tools/x87-guest-test.py            # must print "... identical"
# Super PI 1M in XP, then with -cpu pentium3,x87-fast=off (doc 00 §benchmarks)
```

If the aarch64 lowering is wrong the guest test will show mismatches or
QEMU will abort in `tcg_out_op`; the fallback is `-cpu …,x87-fast=off`.
