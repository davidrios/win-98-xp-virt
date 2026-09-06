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
else
  export MTOOLS_SKIP_CHECK=1
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9x.drv" ::/WINDOWS/SYSTEM/D3DPT9X.DRV
  mcopy -i "$RAW@@$OFF" -o "$DRV/d3dpt9v.vxd" ::/WINDOWS/SYSTEM/D3DPT9V.VXD
fi

rm -f "$OUT/out/dbg.log" "$OUT/out/stderr.log"
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

sleep 6;  bars bios
sleep 30; bars 30s
sleep $((BOOT_WAIT - 36)); bars boot

python3 "$ROOT/tools/qmpc.py" "$SOCK" screendump "$OUT/out/screen.ppm" >/dev/null
echo "colours   $(identify -format '%wx%h %k' "$OUT/out/screen.ppm.ppm" 2>/dev/null || echo '?')  ($OUT/out/screen.png)"
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
# Escape first: a modal dialog swallows the power button too, and an
# `install` run always ends on one ("you must restart your computer" —
# Escape is No, and the restart is the next run's job).
python3 "$ROOT/tools/qmpc.py" "$SOCK" keys esc >/dev/null 2>&1 || true
sleep 3
python3 "$ROOT/tools/qmpc.py" "$SOCK" json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
for _ in $(seq 30); do kill -0 $VM 2>/dev/null || break; sleep 3; done

if kill -0 $VM 2>/dev/null; then
  echo "           the power button was ignored; trying the Start menu"
  for k in ret esc ctrl+esc u ret; do
    python3 "$ROOT/tools/qmpc.py" "$SOCK" keys $k >/dev/null 2>&1 || true
    sleep 4
  done
  for _ in $(seq 30); do kill -0 $VM 2>/dev/null || break; sleep 3; done
fi

if kill -0 $VM 2>/dev/null; then
  echo "shutdown   the machine did not power off — the next boot will be safe mode"
else
  echo "shutdown   clean"
fi
