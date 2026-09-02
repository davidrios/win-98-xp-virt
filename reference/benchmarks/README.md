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
  -vga std -net none -usb -device usb-tablet -device AC97,audiodev=embed0 \
  -cdrom ~/vms/bench.iso
```

Get the benchmark binary into the guest on an ISO (no network in the
reference machine):

```sh
mkdir bench && cp super_pi_mod-1.5.zip bench/     # unzip inside the guest
hdiutil makehybrid -o ~/vms/bench.iso -iso -joliet bench      # macOS
# Linux: mkisofs -o bench.iso -J -r bench
```

On the rig, run the same zip from the same ISO burned to a CD-R or copied
over; XP is the dual-boot partition. Note the rig's actual CPU/clock and RAM
in the table.

## Results

| Machine | CPU / config | Super PI 1M (s) | 7-Zip 26.02 GIPS (comp / decomp) | Boot to desktop (s) | Date |
|---|---|---|---|---|---|
| Rig (P4 + 6200), XP | Pentium 4 1.7 GHz (Willamette), 512 MB | 122 (2:02) | 0.742 / 0.776 | ~30 | 2026-09-02 |
| M1 Air, XP in player, TCG `-cpu pentium3 -m 512` | Apple M1, macOS 26 | 589 (9:49) | 0.996 / 1.511 | ~30 | 2026-09-02 |
| Air ÷ rig | | **4.8× slower → 21 %** | **134 % / 195 %** | parity | |

Reading: the two tests bracket TCG on the M1 against a real P4 1.7.
- **Integer/memory (7-Zip):** the emulated XP is *faster* than the rig —
  1.3–2× a P4 1.7, i.e. a 2.2–3 GHz P4 / early Athlon 64 class.
- **x87 FP (Super PI):** 21 % of the rig. TCG runs every x87 op through
  80-bit softfloat with no fast path; 9:49 for 1M is Pentium II 300–400 MHz
  territory. This is the worst case, and it is what FP-heavy game code and
  software renderers hit.
- Boot time is disk/interrupt bound and the host SSD hides the rest.

In-app expectation text should say both halves: "integer speed of a fast
P4; floating-point speed of a Pentium II". Games mix the two; per-title
validation in M4 decides which side dominates.

Also record here anything the XP guest needed that Win98 did not
(drivers, HAL, activation state is not recorded).
