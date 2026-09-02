# CPU benchmarks: XP under TCG vs. the reference rig

Purpose (doc 06, doc 08 M1 exit): state XP-on-Apple-Silicon performance as a
percentage of the real P4 + GeForce 6200 rig, from the same binaries run the
same way. CPU-only here; 3D benchmarks (3DMark) wait for M3/M4 when the
player presents qemu-3dfx output.

## Tests

| Test | Why | Record |
|---|---|---|
| Super PI mod 1.5 XS, 1M digits | single-thread integer/FPU, era-standard, runs on 98/XP | seconds (lower is better), 2 runs, best |
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

| Machine | CPU / config | Super PI 1M (s) | Boot to desktop (s) | Feel | Date |
|---|---|---|---|---|---|
| Rig (P4 + 6200), XP SP3 | fill in: P4 model, GHz, RAM | | | | |
| M1 Air, XP SP3 in player, TCG `-cpu pentium3 -m 512` | Apple M1, macOS 26 | | | | |
| Rig / Air | | = Air ÷ rig → **X % of the reference P4** | | | |

Also record here anything the XP guest needed that Win98 did not
(drivers, HAL, activation state is not recorded).
