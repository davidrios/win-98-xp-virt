#!/usr/bin/env bash
# xp-driver-test.sh — drive an XP image through the d3dpt-vga driver tests
# headlessly (doc 15). Boots standalone QEMU (KVM when /dev/kvm exists)
# with the driver ISO and a FAT scratch disk, types commands over QMP, and
# pulls the guest logs out of the scratch disk with mtools.
#
#   tools/xp-driver-test.sh <image.qcow2> install      # DRVINST from the ISO, reboot, desktop on the driver
#   tools/xp-driver-test.sh <image.qcow2> ddtest       # DDTEST 640x480x16 / x32 / windowed, logs + BMP
#   tools/xp-driver-test.sh <image.qcow2> modes        # SETMODE 1024x768x32@85, 800x600x16@75, list
#   tools/xp-driver-test.sh <image.qcow2> cmd 'D:\DRIVER\SETMODE.EXE'   # any guest command line
#
# Env: DDFLAGS=N (-device d3dpt-vga,ddflags=N), OUT=dir for screendumps
# and logs (default build/xp-driver-test), NO_KVM=1. Needs
# guest-tools/build-driver.sh run first (guest-tools/out/d3dpt-driver.iso),
# mkfs.fat + sfdisk + mtools + python3. Ends every run with a clean
# power-down. Keys typed while a full-screen DirectDraw window is up are
# lost, so each test is ONE chained "cmd /k a & b & c" command line.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?image.qcow2}"; MODE="${2:?install|ddtest|modes|cmd}"; shift 2
OUT="${OUT:-$ROOT/build/xp-driver-test}"; mkdir -p "$OUT"
ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
[ -f "$ISO" ] || { echo "no $ISO: run guest-tools/build-driver.sh"; exit 1; }
SCRATCH="$OUT/scratch.img"
if [ ! -f "$SCRATCH" ]; then
  truncate -s 64M "$SCRATCH"
  printf 'label: dos\nstart=2048, type=0c\n' | sfdisk -q "$SCRATCH"
  mkfs.fat -F 32 --offset 2048 "$SCRATCH" >/dev/null
fi
SOCK="$OUT/qmp.sock"; rm -f "$SOCK"
ACCEL=(-cpu pentium3)
[ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu host)
LOG="$OUT/qemu-$MODE.log"
"$ROOT/build/qemu/qemu-system-i386" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc -m 512 \
  -hda "$IMG" -hdb "$SCRATCH" -cdrom "$ISO" -vga none -device "d3dpt-vga,ddflags=${DDFLAGS:-0}" \
  -net none -usb -device usb-tablet -display none -qmp "unix:$SOCK,server,nowait" \
  -serial none -monitor none > "$LOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
run() {  # one chained guest command line in a console that stays open
  Q keys esc; sleep 1; Q keys meta_l+r; sleep 2
  Q type "cmd /k $1"; Q keys ret
}
pull() { mcopy -n -i "$SCRATCH@@1048576" "::/$1" "$OUT/$1" 2>/dev/null && echo "-- $1" && cat "$OUT/$1"; }
finish() {
  Q screendump "$OUT/$MODE-end.png" || true
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  sleep 20; wait $QPID || true
  echo "---- $LOG (device side)"; grep -v "^WARNING" "$LOG" | sed 's/qemu-system-i386: info: //' | tail -40
}

sleep "${BOOT_WAIT:-45}"
case "$MODE" in
  install)
    run 'D:\DRIVER\DRVINST.EXE'
    sleep 15; Q screendump "$OUT/install-done.png"
    Q type 'shutdown -r -t 0'; Q keys ret
    sleep 60; Q screendump "$OUT/install-rebooted.png"
    finish ;;
  ddtest)
    run 'D:\DRIVER\DDTEST.EXE 640 480 16 300 & copy ddtest.log E:\dd16.log & copy ddtest.bmp E:\dd16.bmp & D:\DRIVER\DDTEST.EXE 640 480 32 300 & copy ddtest.log E:\dd32.log & D:\DRIVER\DDTEST.EXE 640 480 32 200 -windowed & copy ddtest.log E:\ddwin.log'
    sleep 6; Q screendump "$OUT/ddtest-fullscreen.png"
    sleep 50
    finish
    pull dd16.log; pull dd32.log; pull ddwin.log
    mcopy -n -i "$SCRATCH@@1048576" ::/dd16.bmp "$OUT/dd16.bmp" 2>/dev/null || true ;;
  modes)
    run 'D:\DRIVER\SETMODE.EXE 1024 768 32 85 & D:\DRIVER\SETMODE.EXE 800 600 16 75 & D:\DRIVER\SETMODE.EXE 1024 768 32 85 & D:\DRIVER\SETMODE.EXE > E:\modes.log'
    sleep 25
    finish
    pull modes.log ;;
  cmd)
    run "${1:?guest command line}"
    sleep "${CMD_WAIT:-30}"
    finish ;;
  *) echo "unknown mode $MODE"; kill $QPID; exit 1 ;;
esac
