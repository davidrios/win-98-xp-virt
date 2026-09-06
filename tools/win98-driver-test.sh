#!/usr/bin/env bash
# The Win98/Me display driver in a real guest, headless (doc 19, M10) —
# the 9x counterpart of tools/xp-driver-test.sh, at the stage the driver
# is at: install it, boot the machine on `-vga none -device d3dpt-vga`,
# and report what the adapter and the screen say.
#
#   tools/win98-driver-test.sh ~/vms/win98.qcow2 [boot|install]
#
# `install` stages d3dpt9x.drv and its INF into the image's WINDOWS\INF so
# that PnP matches PCI\VEN_1234&DEV_3D00 on the next boot and installs the
# driver with no clicks; `boot` (the default) just boots and looks.
#
# **Never touches the user's image**: it converts a copy to raw once
# (build/w98/win98-m10.raw) and works on that, because mtools cannot write
# into a qcow2 and the driver has to be staged from outside — there is no
# in-guest shell to drive before the display works.
#
# What it prints, and what each line means:
#   BARs        the adapter's PCI base addresses over the boot. The BIOS
#               maps them; if Windows unmaps them, nothing claimed the
#               resources — that is the mini-VDD's job (doc 19), and the
#               16-bit driver then has nothing to map.
#   guest:      the driver's own debug output, through the adapter's DEBUG
#               register into the QEMU log, exactly as on XP.
#   0xE9        the same lines through QEMU's debug console, which works
#               before the register page is mapped — when this is empty
#               too, no code of ours ran at all.
#   colours     the screendump's colour count: 16 or fewer means Windows
#               fell back to VGA and the driver is not driving the screen.
#
# `BOOT_WAIT=<s>` buys more time on a slow run, `SHOTS=<s>` adds a screendump
# every <s> seconds (`t<n>.png` in the output directory) so that a screen
# which is merely filling in slowly can be told from one that stopped
# changing, and `OUT=` moves the outputs.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

IMG="${1:?usage: win98-driver-test.sh <win98.qcow2> [boot|install]}"
WHAT="${2:-boot}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/build/w98}"
DRV="$ROOT/guest-tools/out/driver9x"
QEMU="${QEMU_BIN:-$ROOT/build/qemu/qemu-system-i386}"
RAW="$OUT/win98-m10.raw"
SOCK="/tmp/claude-$(id -u)/w98m10.sock"
BOOT_WAIT="${BOOT_WAIT:-150}"

[ -x "$QEMU" ] || { echo "no QEMU at $QEMU (QEMU_BIN= to point elsewhere)"; exit 1; }
[ -f "$DRV/d3dpt9x.drv" ] || { echo "run guest-tools/build-driver9x.sh first"; exit 1; }
mkdir -p "$OUT/out" "$(dirname "$SOCK")"

if [ ! -f "$RAW" ] || [ "$WHAT" = install ]; then
  echo "==> raw copy of $IMG (the user's image is never written)"
  rm -f "$RAW"
  "${QEMU_IMG:-$ROOT/build/qemu/qemu-img}" convert -O raw "$IMG" "$RAW"
fi

# the FAT16/32 partition, from the MBR
OFF=$(python3 -c "
import struct,sys
m=open('$RAW','rb').read(512)
for i in range(4):
    e=m[446+i*16:446+(i+1)*16]
    if e[4]: print(struct.unpack_from('<I',e,8)[0]*512); break")

if [ "$WHAT" = install ]; then
  export MTOOLS_SKIP_CHECK=1
  echo "==> staging the driver and its INF (PnP installs it on the next boot)"
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9x.drv" ::/WINDOWS/SYSTEM/D3DPT9X.DRV
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9v.vxd" ::/WINDOWS/SYSTEM/D3DPT9V.VXD
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9x.drv" ::/WINDOWS/INF/D3DPT9X.DRV
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9v.vxd" ::/WINDOWS/INF/D3DPT9V.VXD
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9x.inf" ::/WINDOWS/INF/D3DPT9X.INF

  # PnP writes the driver and the mini-VDD into the registry, and Windows is
  # then supposed to find both from there — `display.drv=pnpdrvr.drv` in the
  # `[boot]` section resolves through the PnP device's own key. **That path
  # has never been seen to work here**: the boot after a clean install comes
  # up on the VGA with neither the VxD's nor the driver's debug lines, while
  # naming both in SYSTEM.INI works every time. So the install names them,
  # which is also what the earlier runs of this track proved the driver on —
  # it was hand-edited into the image then, and this is the same thing done
  # reproducibly. Whether the registry path can be made to work is a
  # question for later (doc 19 Section 16); it is not a thing to discover
  # again by accident.
  #
  # **In binary, or not at all.** SYSTEM.INI has CRLF line endings and
  # Python's text mode eats them on the way through, which has already cost
  # this track a section header and the run that noticed.
  echo "==> naming the driver and the mini-VDD in SYSTEM.INI"
  mcopy -i "$RAW@@$OFF" -n ::/WINDOWS/SYSTEM.INI "$OUT/system.ini"
  python3 - "$OUT/system.ini" <<'PYINI'
import re, sys
p = sys.argv[1]
b = open(p, 'rb').read()

def section(name):
    m = re.search(br'^\[' + name + br'\]\r?\n', b, re.M | re.I)
    if not m:
        sys.exit("SYSTEM.INI has no [%s] section" % name.decode())
    return m.end()

# [386Enh] device= for the mini-VDD: the main VDD's `minivdd=` registry
# value is the documented way and is not the way that works here.
if b'D3DPT9V.VXD' not in b.upper():
    at = section(b'386Enh')
    b = b[:at] + b'device=C:\\WINDOWS\\SYSTEM\\D3DPT9V.VXD\r\n' + b[at:]

# [boot] display.drv= for the driver itself, replacing whatever is there.
m = re.search(br'^display\.drv=[^\r\n]*', b, re.M | re.I)
if m:
    b = b[:m.start()] + b'display.drv=d3dpt9x.drv' + b[m.end():]
else:
    at = section(b'boot')
    b = b[:at] + b'display.drv=d3dpt9x.drv\r\n' + b[at:]

open(p, 'wb').write(b)
PYINI
  mcopy -i "$RAW@@$OFF" -o "$OUT/system.ini" ::/WINDOWS/SYSTEM.INI
else
  export MTOOLS_SKIP_CHECK=1
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9x.drv" ::/WINDOWS/SYSTEM/D3DPT9X.DRV
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9v.vxd" ::/WINDOWS/SYSTEM/D3DPT9V.VXD
fi

rm -f "$OUT/out/dbg.log" "$OUT/out/stderr.log" "$OUT/out"/t*.ppm* "$OUT/out"/t*.png
echo "==> booting on -vga none -device d3dpt-vga"
"$QEMU" -L "$ROOT/qemu/pc-bios" -machine pc -m 256 -accel tcg \
  -drive file="$RAW",format=raw,if=ide,index=0 -vga none -device d3dpt-vga \
  -net none -display none -rtc base=localtime \
  -debugcon file:"$OUT/out/dbg.log" -qmp unix:"$SOCK",server,nowait \
  > "$OUT/out/stderr.log" 2>&1 &
VM=$!
trap 'kill $VM 2>/dev/null || true' EXIT

hmp() { python3 "$ROOT/tools/qmpc.py" "$SOCK" json "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"$1\"}}"; }
bars() { hmp "info pci" | python3 -c "
import json,sys
o=json.load(sys.stdin).get('return','').splitlines()
for i,l in enumerate(o):
    if '1234:3d00' in l:
        print('BARs  %-6s %s' % ('$1', ' '.join(x.strip() for x in o[i+1:i+4]))); break"; }

# A screendump every SHOTS seconds, because the interesting question through
# most of this track is not what the screen ends on but *when* it stopped
# changing: a desktop that is merely slow under TCG fills in over the run,
# and one that is never painted does not. Off by default (a dump is a
# millisecond of the guest's time, but a hundred files is noise).
shots() {
  [ -n "${SHOTS:-}" ] || return 0
  local t=$1
  python3 "$ROOT/tools/qmpc.py" "$SOCK" screendump "$OUT/out/t$t.ppm" >/dev/null 2>&1 || true
}

# The boot, in SHOTS-second (or single-jump) steps, with the BARs read at
# the three moments that have ever differed.
sleep 6;  bars bios
step=${SHOTS:-24}
t=6
while [ $t -lt "$BOOT_WAIT" ]; do
  n=$(( t + step > BOOT_WAIT ? BOOT_WAIT - t : step ))
  sleep $n; t=$(( t + n ))
  [ $t -ge 30 ] && [ $(( t - n )) -lt 30 ] && bars 30s
  shots $t
done
bars boot

python3 "$ROOT/tools/qmpc.py" "$SOCK" screendump "$OUT/out/screen.ppm" >/dev/null
echo "colours   $(identify -format '%wx%h %k' "$OUT/out/screen.ppm.ppm" 2>/dev/null || echo '?')  ($OUT/out/screen.png)"

# **What the screen shows is not all Windows is saying.** When the guest
# faults, Windows puts its message up in VGA *text* mode — and the adapter is
# scanning out a linear frame buffer, so nobody sees it: the screendump is a
# black desktop with a wait cursor, which reads exactly like a driver that is
# merely slow. The text is still in VRAM, because QEMU's VGA core keeps its
# planes interleaved four bytes to a character cell from offset 0, which is
# also the top of our frame buffer — the band of coloured noise across the
# first 32 KB of every one of these screendumps *is* the message. So read it.
text_screen() {
  local bar0
  bar0=$(hmp "info pci" | python3 -c "
import json,sys,re
o = json.load(sys.stdin).get('return','').splitlines()
for i,l in enumerate(o):
    if '1234:3d00' in l:
        for m in o[i+1:i+4]:
            g = re.search(r'prefetchable memory at 0x([0-9a-f]+)', m)
            if g: print(int(g.group(1), 16)); break
        break")
  [ -n "$bar0" ] || return 0
  python3 "$ROOT/tools/qmpc.py" "$SOCK" json \
    "{\"execute\":\"pmemsave\",\"arguments\":{\"val\":$bar0,\"size\":32768,\"filename\":\"$OUT/out/vram.bin\"}}" >/dev/null 2>&1 || return 0
  python3 - "$OUT/out/vram.bin" <<'PYTXT'
import sys
v = open(sys.argv[1], 'rb').read()
rows = []
for r in range(25):
    line = ''.join(chr(v[(r * 80 + c) * 4]) if 32 <= v[(r * 80 + c) * 4] < 127 else ' '
                   for c in range(80))
    rows.append(line.rstrip())
if not any(rows):
    sys.exit(0)
print("text      Windows has a VGA text screen up behind the frame buffer:")
for line in rows:
    if line: print("          | " + line)
PYTXT
}
text_screen
echo "0xE9      $(wc -c < "$OUT/out/dbg.log") bytes"
sed 's/^/          /' "$OUT/out/dbg.log" | head -20
echo "guest:    $(grep -c 'd3dpt-vga: guest' "$OUT/out/stderr.log" || true) lines"
grep 'd3dpt-vga' "$OUT/out/stderr.log" | sed 's/^/          /' | head -20 || true

# A killed Win98 leaves the FAT dirty, and the boot after that comes up in
# **safe mode** with no driver and no VxD — which looks exactly like the
# driver having failed, and costs a whole run to work out. The ACPI power
# button is the reliable way to end a run: this is an ACPI install (it has
# to be, or the adapter is never seen), and Windows shuts down and powers
# the machine off by itself, with no dependence on what is on screen. The
# Start menu is the fallback for when it does not, and it starts by
# dismissing whatever modal dialog may be swallowing the keys.
echo "==> shutdown"
# The order matters and each step is here for a run it cost. `alt+n` answers
# the one dialog this harness *knows* is up at the end of an `install` ("to
# finish setting up your new hardware, you must restart your computer" — No,
# because the restart is the next run's job, and Escape alone has been seen
# not to reach it). Escape then clears anything else modal, because a dialog
# swallows the power button as surely as it swallows keys. Only then the ACPI
# button, which is the reliable one: this is an ACPI install (it has to be, or
# the adapter is never seen) and Windows powers the machine off by itself with
# no dependence on what is on screen.
for k in alt+n esc; do
  python3 "$ROOT/tools/qmpc.py" "$SOCK" keys $k >/dev/null 2>&1 || true
  sleep 3
done
python3 "$ROOT/tools/qmpc.py" "$SOCK" json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
for _ in $(seq 40); do kill -0 $VM 2>/dev/null || break; sleep 3; done

if kill -0 $VM 2>/dev/null; then
  echo "           the power button was ignored; trying the Start menu"
  for k in ret esc ctrl+esc u ret; do
    python3 "$ROOT/tools/qmpc.py" "$SOCK" keys $k >/dev/null 2>&1 || true
    sleep 4
  done
  for _ in $(seq 40); do kill -0 $VM 2>/dev/null || break; sleep 3; done
fi

if kill -0 $VM 2>/dev/null; then
  # What is on the screen *now* is the only evidence of what refused to go
  # away, and without it the next run is spent finding out.
  python3 "$ROOT/tools/qmpc.py" "$SOCK" screendump "$OUT/out/stuck.ppm" >/dev/null 2>&1 || true
  echo "shutdown   the machine did not power off — the next boot is a ScanDisk"
  echo "           or safe mode. What was on screen: $OUT/out/stuck.png"
else
  echo "shutdown   clean"
fi
