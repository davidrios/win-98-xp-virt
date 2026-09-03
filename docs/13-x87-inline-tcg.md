# 13. x87 shadow doubles: the FPU stack as host doubles in TCG (2026-09-02)

Patch 05 (doc 00, `patches/qemu/README.md`) moved the common x87 case onto
the host FPU but still paid a helper call and an 80-bit round trip through
`env->fpregs[]` per instruction. This doc records the two steps taken past
it on branch `worktree-x87-inline-tcg`, patch `06-x87-inline-tcg`:
first an inline-per-instruction prototype (kept as the "level 2" data
point below), then the design that shipped in the patch: the translator
keeps the x87 stack as doubles across instructions.

## Where the time went with patch 05

Profile of the 7-op DOS loop (`tools/x87-guest-test.py` bench: fld m64,
fmul m64, fadd m64, fstp m64, fld m64, fdiv m64, fistp m32), x86-64 host:

| Symbol | Share |
|---|---|
| `x87_binop` (the host-FPU body incl. checks and TwoSum) | 33 % |
| `merge_exception_flags` | 11 % |
| individual `helper_f*` (call frame, flag save, FT0/ST0 round trip) | 45 % |
| translated code (TLB lookups, FIP/FDP stores, calls) | ~2 % |

98 % in helpers; forcing the body to inline into them gave 10 %. The cost
is the structure: every x87 op is a call, the value passes through
`env->ft0` / `env->fpregs[]` as floatx80, flags are saved and merged.

## Results (x86-64, Ryzen 7 5700X; ns per x87 op in the loop above)

| Configuration | ns/op | vs softfloat |
|---|---|---|
| softfloat (`x87-fast=off`) | 21.6 | 1.0× |
| patch 05 (helpers on the host FPU) | 10.6 | 2.0× |
| level 2: ops inline, x80 round trip per instruction (prototype) | 6.9 | 3.1× |
| **patch 06: shadow doubles across instructions** | **2.9** | **7.4×** |

With patch 06, 93 % of the time is in translated code; the remaining
helper is nothing in this loop (fistp is inlined too). The fast path of
`fmul m64` is ~45 host instructions: TLB lookup 9, operand window check 5,
multiply + result check with the underflow test folded in 9, FMA residual
+ PE 9, FIP/FDP 7, one sync store of the shadow global.

Correctness: `tools/x87-guest-test.py` (68 instruction sequences × 44²
operand pairs × 7 control words, incl. multi-instruction chains, fcmov,
compares, integer and float conversions), 382,250 result lines identical
with the fast path on and off, plus a 64-instruction block that must stay
one TB. `tools/x87-unwind-test.asm` (linux-user) faults with two dirty
shadows outstanding and verified the unwind repair (saved FPU
state in the signal frame identical to the helper path).

## What "inline in TCG" needed (both levels)

TCG had no float opcodes, but everything around them exists: the
register allocator handles vector registers (gvec), `tcg_out_mov` moves
i64 values between general and vector registers in both backends,
`tcg_out_ld/st` accept an i64 temp living in a vector register. A scalar
float op is therefore an ordinary opcode on i64 temps whose operand
constraint is the vector class:

- `include/tcg/tcg-opc.h`: `add/sub/mul/div_f64`, `fmsub_f64` (a·b − c,
  fused), `sqrt_f64`, `cvt_f64_f32`, `cvt_f32_f64`, gated by
  `TCG_TARGET_HAS_f64`.
- x86-64: `vaddsd/vsubsd/vmulsd/vdivsd/vfmsub213sd/vsqrtsd/vcvtsd2ss/vcvtss2sd`,
  constraints `C_O1_I2(x, x, x)` etc.; requires AVX + FMA3 (new
  `CPUINFO_FMA`).
- aarch64: `fadd/fsub/fmul/fdiv/fnmsub/fsqrt/fcvt` (scalar D, fcvt both ways),
  `C_O1_I2(w, w, w)` etc., plus `tcg_out_movi` into a V register.
  See "Bring-up on the Air" below: two aarch64-only bugs found so far,
  both in code paths upstream never executes.
- Other hosts / TCI: `TCG_TARGET_HAS_f64` is 0, the translator emits the
  helper calls as before.

## The shadow-double design (`target/i386/tcg/x87-shadow.c.inc`)

**Mode.** Inline mode is a TB flag (`TB_FLAG_X87_MODE`, bits 29–30 of
`tb->flags`, from `env->x87_fast_mode`: 1 for PC=53, 2 for PC=24, both
with RC=nearest, PE masked, property `x87-fast` on). Everything that can change the control word
(`fldcw`, `fldenv`, `frstor`, `fninit`, `fnsave`, `fxrstor`, `xrstor`)
ends the TB in both modes. A one-load runtime guard at the first inlined
instruction of each TB protects against a missed case by exiting to the
same instruction (the lookup then finds the right variant). PC=64 code
and other rounding modes run the patch 05 helpers; PC=24 is mode 2, see
"PC=24 mode" below.

**Stack model.** The translator addresses the stack relative to the top
at the first x87 instruction of the TB (a runtime `entry_top` loaded
once): relative slot r is physical register `(entry_top + r) & 7`, and
`ST(i)` is slot `(delta + i) & 7` where `delta` counts pushes and pops.
This costs no TB-flag bits and no TB variants per stack depth. Each slot
is statically MEM (only `fpregs[]` holds it), CLEAN (shadow equals
memory) or DIRTY (shadow newer). Shadows are eight TCG globals
(`cpu_x87_sd[]`, backed by `env->x87_sd[]`). `env->fpstt` and
`env->fptags` stay current in memory (one store per push/pop), so
helpers and the unwinder always see the real stack layout.

**Invariants.** No DIRTY slot at any TB exit or before any helper that
may read `fpregs[]`; helpers that may write `fpregs[]` or move the top
(all non-inlined x87 instructions, MMX entry, `fxrstor`/`xrstor`) get a
boundary: flush, forget shadows, reset the frame. Exit hooks live in
`gen_eob`, `gen_jmp_rel`, `gen_exception`, `gen_interrupt` and the
`hlt`/`pause`/`mwait`/`vmrun`/`icebp`/`rdpmc` sites (flush code emitted
without touching the static state, so conditional exits inside a TB
stay correct).

**Faults mid-TB.** A guest load or store can fault with DIRTY shadows.
TCG already syncs globals to memory before every `qemu_ld/st`, so the
shadow values are in `env->x87_sd[]`; the insn_start word (a third
`TARGET_INSN_START_EXTRA_WORDS` word: dirty mask + delta) lets
`x86_restore_state_to_opc()` convert them back into `fpregs[]` during
unwind. Inlined memory instructions do their guest access before any
architectural change, so the word for the faulting instruction describes
exactly the state to repair.

**Fast path.** Shadows hold only zero or normal doubles with exponents
within ±900 (the patch 05 window), so arithmetic operands need no checks
and results are checked once (window; underflow for mul/div; divisor
nonzero). The inexact flag comes from the TwoSum/FMA residual and is
ORed into `fpus`. Everything is branchless except the branches to the
slow block, and the fast path contains no labels, so TCG keeps its
globals (and the shadows) in registers across instructions.

**Slow blocks.** Every instruction records one out-of-line block
(`X87SlowBlock`: static stack state at instruction start, pc/cc state,
operands as TB temps). At `tb_stop` the blocks are emitted after the
normal exit: write back all dirty shadows, run the unmodified helper
sequence (including the store and pop for `fist`/`fst`), update FIP/FDP,
exit the TB to the next instruction. Guest code that hits a slow case in
a hot loop (NaN/inf/denormal operands, values outside ±2^900, 64-bit
mantissas from `fldpi`/`fsin` results, `fst m32` overflow, `fist` out of
range) pays a TB exit per occurrence: bounded (a helper plus a TB
transition) but worth knowing.

**Deviation, on purpose.** A popped register keeps its previous
`fpregs[]` content instead of the popped value: materializing every
popped slot would cost ~20 host instructions per `fstp` for something
only observable by reading an empty register (which QEMU never faults
on, and real hardware answers with an indefinite NaN). `fxsave` images of
empty registers can differ from patch 05's; round trips through
`fxsave`/`fxrstor` (context switches) are unaffected.

## Coverage and fallbacks

Inlined: `fld/fst/fstp m32/m64`, `fild/fist/fistp m16/m32`, all six
arithmetic ops with m32/m64/st(i) operands including the popping and
reversed forms, `fld/fst/fstp st(i)`, `fxch`, `fcmovcc`, `fld1`,
`fldz`, `fchs`, `fabs`, `fsqrt`, `frndint`, `fcom/fcomp/fcompp`,
`fucom/fucomp/fucompp`, `fcomi/fucomi(p)`, `ftst`, `ffree(p)`,
`fincstp/fdecstp`, `fnstsw ax`. Helpers with a boundary: `fld/fstp m80`,
`fild/fistp m64`, `fisttp`, `fbld/fbstp`, transcendentals, `fscale`,
`fprem`, `fxtract`, `fxam`, the other constants, `fnsave/frstor`,
`fnstenv/fldenv`. No boundary needed: `fnstcw`, `fnstsw m16`, `fnclex`,
`fwait`. A TB with more than 94 inlined x87 instructions falls back to
helpers for the rest.

## Bring-up on the Air (state on 2026-09-03, resume here)

Everything above is verified on x86-64. On the M1 the patch has run
twice so far; each run found one aarch64 backend path that upstream
QEMU never executes because nothing upstream keeps an i64 temp in a
vector register. Both are host-side illegal instructions (SIGILL in the
vCPU thread, `cpu_tb_exec` at the top of the crash report); the
exception code in the macOS crash report is the instruction word, which
identifies the emitter.

1. XP in the player: `0x4e003c19` = `UMOV x25, v0.d[0]` with a zero
   element-size field. `tcg_out_mov` vector→general passed 0 for imm5.
   Fixed (`4 << type`), encoding checked against llvm-mc. Commit e70f92e.
2. Rebuilt correctly (`scripts/prepare-qemu.sh`, configure, ninja for
   both `qemu-system-i386` and the dylib), `tools/x87-guest-test.py` now
   runs inline code and dies at result index 1422 (reported as "line
   1424", the LONG and CW lines count): `op_fld_st` with a = +0, i.e.
   `fld tword [pool]; fld1; fld st1; fmulp st1, st0`. Every binop with
   every operand pair and the unary ops before it passed, so arithmetic,
   reloads, compares, fxch and fcmov are fine on aarch64. What is new in
   this sequence is `fld1`: it puts a TCG constant into a shadow and
   `fmulp` is the first f64 opcode fed a constant operand, which goes
   through the `tcg_out_movi`-into-a-V-register fallback I added (it
   used `tcg_out_dupi_vec`). That fallback is now replaced by "movi into
   `TCG_REG_TMP0`, then `INS Vd.D[0], Xtmp`" whose encoding is checked
   against llvm-mc (`mov v0.d[0], x25` = `4e081f20`). Untested on the Air.
   If the next run still dies there, get the instruction word from the
   crash report (or run `qemu-system-i386 ... -d out_asm -D log` on the
   test floppy and look at the TB for `fld1`).

The earlier "guest test passes on the Air" was a stale
`qemu-system-i386`; the script now prints a warning when the on/off
bench ratio is under 1.5x (fast path inactive). Expect ~5x or more.

Order on the Mac: `git pull`, prepare → configure → ninja (both
targets) → `cargo build --release`; `python3 tools/x87-guest-test.py`
(must end with "... identical" and no warning); the linux-user unwind
test cannot run on macOS (skip it); then XP in the player, Super PI 1M
twice (also with `-cpu pentium3,x87-fast=off`), Win98 boot and feel
check; fill in `reference/benchmarks/README.md`; merge to main.

Result (2026-09-03, both aarch64 fixes in): XP Super PI 1M on the Air
1:57, twice (9:49 on softfloat; the `x87-fast=off` control was at 0:35
after loop 1, softfloat pace). That beats the rig's real P4 1.7 (2:02).
Win98 boots and feels fine. Recorded in `reference/benchmarks/README.md`.

## PC=24 mode (2026-09-03)

Direct3D sets precision control to 24 bits when it creates a device
(unless the app passes the preserve-FPU flag), so most D3D-era game code
runs the whole frame at PC=24. Until now those TBs fell back to the
patch 05 helpers. Mode 2 of the inline path covers them with the same
shadows, not binary32 ones:

- `env->x87_fast_mode` is 0, 1 (PC=53) or 2 (PC=24, RC=nearest, PE
  masked); it is a 2-bit TB flag (`TB_FLAG_X87_MODE`, bits 29–30) and
  the runtime guard compares against the TB's mode.
- Shadows stay binary64 but hold only values with a 24-bit significand
  inside the float window (`X87S_WIN24`, exponent ±126). A reload from
  `fpregs[]` requires the low 40 mantissa bits clear (as `x87f_x80_to_f`),
  m32 loads and the constants have it, and every arithmetic result is
  rounded to it by `x87s_round24`: convert the host binary64 result to
  binary32 and back (new opcode `cvt_f32_f64`: `vcvtss2sd` / `fcvt d, s`).
  For 24-bit operands that double rounding equals the correctly rounded
  24-bit result of +, −, ×, ÷ and sqrt (53 ≥ 2·24+2), so the arithmetic
  code is shared and the rounding step is the only addition. PE is the
  residual test ORed with "rounding changed the bits".
- Softfloat rounds m64 operands to 24 bits at that precision and raises
  PE (`float64_to_floatx80`; real hardware never rounds loads, patch 05
  already defers to softfloat there): `fld/fxxx/fcom m64` round the
  operand the same way inline. Integer loads stay exact in both paths,
  so an `fild m32` whose value needs more than 24 bits takes a new slow
  block (`X87S_SLOW_FILD`); `fild m16` always fits.
- Flush, unwind repair (`x87f_d_to_x80`), fist, frndint, fst m32/m64,
  compares, fxch, fcmov are unchanged: they see ordinary doubles.

Verified with `tools/x87-guest-test.py` (the control-word sweep already
included PC=24 RNE and PC=24 up): 382,251 result lines identical on/off
on the M1 Air. The bench now runs at both precisions; on the Air the
7-op m64 loop is 0.38 s at PC=53 (10.4× softfloat) and 0.66 s at PC=24
(6.0×): every m64 operand pays the rounding and its PE store, which
float (m32) game code does not. Real-world check still to do: a D3D
title in XP with `x87-fast=off` as the control.

## Follow-ups

- `-cpu pentium3,x87-fast=off` stays as the fallback if something
  misbehaves in a Windows guest.
- Per-op cost (~45 host instructions) can still drop: defer FIP/FCS to
  flush points with the pc in the insn_start word (~4), keep FDP eager.
  At PC=24, native binary32 opcodes would save the two conversions per
  op if a profile of a D3D game says the x87 share still matters.
- Slow-path frequency counters (`-d` or a trace) would tell whether any
  real workload exits often enough to matter.
