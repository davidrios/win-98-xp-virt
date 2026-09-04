# CPU benchmarks: XP under TCG vs. the reference rig

Purpose (doc 06, doc 08 M1 exit): state XP-on-Apple-Silicon performance as a
percentage of the real P4 + GeForce 6200 rig, from the same binaries run the
same way. CPU-only here; 3D benchmarks (3DMark) wait for M3/M4 when the
player presents qemu-3dfx output.

## Tests

| Test | Why | Record |
|---|---|---|
| Super PI mod 1.5 XS, 1M digits | single-thread x87 FP, era-standard, runs on 98/XP | seconds (lower is better), 2 runs, best |
| 7-Zip benchmark (Tools → Benchmark) | integer + memory, single thread in a 1-CPU guest | compress / decompress GIPS |
| Boot to desktop | disk + interrupt-heavy; user-visible | seconds from power-on to a responsive Start menu |
| Feel check | scrolling in Explorer, Notepad typing, Minesweeper, window drag | note anything that stutters |

Run everything twice and take the best. Nothing else running on the host.

## Player command (Air)

```sh
PLAYER_LATENCY=1 target/release/player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- \
  -L $PWD/qemu/pc-bios -machine pc -cpu pentium3 -m 512 -hda ~/vms/xp.qcow2 \
  -vga cirrus -net none -usb -device usb-tablet -device AC97,audiodev=embed0 \
  -cdrom ~/vms/bench.iso
```

(The XP runs up to 2026-09-03 used `-vga std`, for which XP has no driver:
basic 640×480×16 VGA. Irrelevant to Super PI and 7-Zip; cirrus from now on.)

Get the benchmark binary into the guest on an ISO:

```sh
mkdir bench && cp super_pi_mod-1.5.zip bench/     # unzip inside the guest
hdiutil makehybrid -o ~/vms/bench.iso -iso -joliet bench      # macOS
# Linux: mkisofs -o bench.iso -J -r bench
```

On the rig, run the same zip from the same ISO burned to a CD-R or copied
over; XP is the dual-boot partition. Results come back over the LAN:
`python3 tools/upload-server.py` on the Mac serves a plain upload form
(`http://<mac>:8000/`, no JavaScript, IE6 works) and saves into
`build/uploads/`. Note the rig's actual CPU/clock and RAM
in the table.

## Results

| Machine | CPU / config | Super PI 1M (s) | 7-Zip 26.02 GIPS (comp / decomp) | Boot to desktop (s) | Date |
|---|---|---|---|---|---|
| Rig (P4 + 6200), XP | Pentium 4 1.7 GHz (Willamette: XP reports "Intel Pentium 4 CPU 1.70GHz", CPUID family 15 model 1), 512 MB | 122 (2:02) | 0.742 / 0.776 | ~30 | 2026-09-02 |
| M1 Air, XP in player, TCG `-cpu pentium3 -m 512` | Apple M1, macOS 26 | 589 (9:49) | 0.996 / 1.511 | ~30 | 2026-09-02 |
| M1 Air, same + patch 05 x87 fast path (`x87-fast=on`, default) | Apple M1, macOS 26 | 393 (6:33) — loop 1: 23 s vs 36 s off | | | 2026-09-02 |
| M1 Air, same + **patch 06** (x87 stack as host doubles in TCG) | Apple M1, macOS 26 | 117 (1:57), both runs — loop 1 at 0:35 with `x87-fast=off` on the same build | | | 2026-09-03 |
| Air ÷ rig | | **21 % → 31 % with patch 05 → 104 % with patch 06** | **134 % / 195 %** | parity | |

Reading: the two tests bracket TCG on the M1 against a real P4 1.7.
- **Integer/memory (7-Zip):** the emulated XP is *faster* than the rig —
  1.3–2× a P4 1.7, i.e. a 2.2–3 GHz P4 / early Athlon 64 class.
- **x87 FP (Super PI):** 21 % of the rig. TCG runs every x87 op through
  80-bit softfloat with no fast path; 9:49 for 1M is Pentium II 300–400 MHz
  territory. This is the worst case, and it is what FP-heavy game code and
  software renderers hit.
- Boot time is disk/interrupt bound and the host SSD hides the rest.

**Patch 05 (x87 host-FPU fast path, 2026-09-02):** Super PI 1M on the Air
9:49 → 6:33 (1.5×; initial 8 s → 5 s, loop 1 36 s → 23 s, `x87-fast=off`
reproduces the old numbers on the same build). What remained was TCG's
per-instruction cost: every x87 op still a helper call, and every
memory-operand form (`fmul qword [m]`, the bulk of compiler output) two
helpers plus an 80-bit round trip.

**Patch 06 (x87 stack as host doubles in TCG, 2026-09-03):** Super PI 1M
on the Air 6:33 → 1:57 (two runs, identical), 5.0× over softfloat, and
now edges the real P4 1.7 (2:02). Control: the same build with
`-cpu pentium3,x87-fast=off` was at 0:35 after loop 1, i.e. softfloat
pace, so the whole delta is the patch. Win98 boots and behaves in the
player with patch 06 on. The "Pentium II floating point" caveat below
no longer applies with patch 06 on. Not re-run: 7-Zip and boot (integer
path untouched, expected unchanged).

In-app expectation text: with patch 06 both halves are at or above a
P4 1.7 ("integer speed of a fast P4; floating-point parity with the rig").
Without it (softfloat) the FP half is Pentium II class. Games mix the two;
per-title validation in M4 decides which side dominates.

Also record here anything the XP guest needed that Win98 did not
(drivers, HAL, activation state is not recorded).

## SSE (patch 07, doc 16)

Two instruments. `tools/sse-guest-test.py` runs `SSEBENCH.COM` (DOS,
register-only kernels, 40M iterations, BIOS ticks) with `sse-fast=on` and
`off` on the same build; `SSEBENCH.EXE` (guest-tools ISO, from
`guest-tools/src/ssebench.c`) is the Win32 program for the rig and the
guests: D3DX-shaped SSE1 kernels and the same math in x87 C, ns per op.

| Machine / config | packed 8-op chain (s) | scalar 7-op chain (s) | Date |
|---|---|---|---|
| M1 Air, TCG, helpers (`-cpu pentium3,+sse2,sse-fast=off`) | 4.56 | 2.80 | 2026-09-04 |
| M1 Air, TCG, **patch 07** inline (default) | 0.38 (**12×**) | 0.77 (**3.6×**) | 2026-09-04 |

That is ~1.2 ns per packed op and ~2.7 ns per scalar op inline against
~14 and ~10 for the helpers. Loops with memory operands see less (the
TLB lookup per operand is the same on both paths).

`SSEBENCH.EXE` in XP (`tools/xp-ssebench.sh ~/vms/winxp.qcow2`, `-iter 20`,
x87 control word PC=53, best of two passes per boot; ns per op, lower is
better):

| Machine / config | xform | normalize | scalar chain | clamp+cmp | convert | C xform (x87) | C normalize (x87) | denormal decay | MMX blend | Date |
|---|---|---|---|---|---|---|---|---|---|---|
| **Rig (P4 1.7), XP, real hardware** | 1.28 | 1.93 | 4.09 | 1.59 | 2.50 | 0.74 | 4.02 | 1552 | 0.44 | 2026-09-04 |
| M1 Air, XP, defaults (patches 06 + 07 + 08) | **2.1** | **2.0** | **3.9** | **4.7** | **2.3** | **4.2** | **4.6** | 80 | **0.41** | 2026-09-04 |
| M1 Air, XP, `simd-fast=off` (07 on) | 2.4 | 2.4 | 3.8 | 4.6 | 2.3 | 4.2 | 4.3 | 83 | 0.80 | 2026-09-04 |
| M1 Air, XP, `sse-fast=off` (06 + 08's predecessor) | 17.7 | 13.5 | 11.8 | 16.2 | 7.4 | 4.5 | 3.9 | 49 | | 2026-09-04 |
| M1 Air, XP, `sse-fast=off,x87-fast=off` | 17.7 | 13.8 | 12.1 | 16.1 | 6.6 | 45.5 | 47.0 | 47 | | 2026-09-04 |
| Air (defaults) ÷ rig | **61 %** | **97 %** | **105 %** | **34 %** | **109 %** | **18 %** | **87 %** | 19× | **107 %** | |

The rig row (2026-09-04, `reference/benchmarks/rig-2026-09-04/ssebench.log`,
three runs with the same mean, SSE score 2.28 ns per op vs the Air's
3.17): every `check` value is identical to the Air's, so the inline paths
reproduce the P4 bit for bit on these kernels. Speed: the Air with all
three patches is at or above the real P4 on the scalar chain, the
conversions, the MMX blend and the normalize (97–109 %), at 61 % on the
packed transform, and at **34 % on clamp+cmp** (`minps`/`maxps`/`cmpps`:
the P4 does 1.6 ns per op, the Air 4.7 — the one SSE kernel that is
clearly behind, next optimisation target). The x87 C transform is the
other outlier at 18 %: a P4 pipelines plain `fmul`/`fadd` at 0.74 ns per
op while the shadow-double translator costs 4.2 (the x87 normalize with
its `fsqrt`/`fdiv` is at 87 %, and Super PI is at parity, so it is the
cheap-op throughput, not the expensive ops). The P4's denormal penalty is
its own legend: 1552 ns per op on real hardware, 19× slower than the
emulated slow path.

Patch 08 (MMX / SSE integer and permutes inline, `tbl_vec`): the MMX
blend kernel 0.80 → 0.41 (2.0×), the packed transform's four `shufps`
2.38 → 2.10 per float op, normalize 2.41 → 2.03. A first version with
scalar lane stores for the shuffles was *slower* than the helper (2.76
and 2.86): the next instruction's vector load stalled behind four
smaller stores; hence the table-lookup opcode. Runs with the `simd`
rows were separate boots from the `sse-fast=off` rows, so compare
within a pair, not across (the E-core effect, up to 2× between boots).

Reading: patch 07 makes the SSE kernels 3.2–7.4× faster (packed code
gains most: the transform is 4 `mulps` + 4 `addps` + 4 `shufps`, and the
shuffles are still helper calls); patch 06 makes the x87 kernels 10–12×
faster; each switch touches only its own kernels. The denormal kernel is
the slow path by design: every multiply leaves the TB for the helper,
0.6× the plain helper. Measurement notes that cost an afternoon:
(1) the first `convert` kernel let its values overflow past 2^31, so
nearly every `cvttss2si` took the slow path (the `info registers`
counters showed 11.8M exits) — kernels must stay in range; (2) mingw's
CRT starts at x87 PC=64 where no x87 fast path applies, so the benchmark
sets PC=53 itself (`-pc24`/`-pc64` to change); (3) macOS parks the vCPU
thread on an efficiency core for whole runs at times, a uniform ~2×, hence
two passes per boot and best-of. gcc `-O2` also auto-vectorized the "C"
kernels into SSE until a pragma pinned them to x87.
