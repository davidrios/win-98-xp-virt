#!/usr/bin/env bash
# xp-driver-test.sh — drive an XP image through the d3dpt-vga driver tests
# headlessly (doc 15). Boots standalone QEMU (KVM when /dev/kvm exists)
# with the driver ISO and a FAT scratch disk, types commands over QMP, and
# pulls the guest logs out of the scratch disk with mtools.
#
#   tools/xp-driver-test.sh <image.qcow2> install      # DRVINST from the ISO, reboot, desktop on the driver
#   tools/xp-driver-test.sh <image.qcow2> ddtest       # DDTEST 640x480x16 / x32 / windowed, logs + BMP
#   tools/xp-driver-test.sh <image.qcow2> modes        # SETMODE 1024x768x32@85, 800x600x16@75, list
#   tools/xp-driver-test.sh <image.qcow2> d3d7         # D3D7TEST: the DX7 HAL scene, diffed against the host test's frame
#   tools/xp-driver-test.sh <image.qcow2> cmd 'D:\DRIVER\SETMODE.EXE'   # any guest command line
#   tools/xp-driver-test.sh <image.qcow2> bat run.bat                   # a batch file, staged as E:\RUN.BAT (long command lines)
#
# Env: DDFLAGS=N (-device d3dpt-vga,ddflags=N), OUT=dir for screendumps
# and logs (default build/xp-driver-test), NO_KVM=1, GAME_ISO=game.iso (the
# game disc takes the CD-ROM drive the game was installed from, D:; the
# driver ISO moves to the next drive, F: after the E: scratch), and for `cmd` / `bat`:
# CMD_WAIT=s (one wait, default 30) or SHOTS=n SHOT_EVERY=s (n screendumps
# cmd-01.png … every s seconds — for watching a game start; SHOT_KEYS="26:esc"
# presses a key right before screendump n). Needs
# guest-tools/build-driver.sh run first (guest-tools/out/d3dpt-driver.iso),
# mkfs.fat + sfdisk + mtools + python3. Ends every run with a clean
# power-down. Keys typed while a full-screen DirectDraw window is up are
# lost, so each test is ONE chained "cmd /k a & b & c" command line.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?image.qcow2}"; MODE="${2:?install|ddtest|modes|d3d7|cmd|bat}"; shift 2
OUT="${OUT:-$ROOT/build/xp-driver-test}"; mkdir -p "$OUT"
ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
[ -f "$ISO" ] || { echo "no $ISO: run guest-tools/build-driver.sh"; exit 1; }
SCRATCH="$OUT/scratch.img"
if [ ! -f "$SCRATCH" ]; then
  truncate -s 64M "$SCRATCH"
  printf 'label: dos\nstart=2048, type=0c\n' | sfdisk -q "$SCRATCH"
  mkfs.fat -F 32 --offset 2048 "$SCRATCH" >/dev/null
fi
if [ "$MODE" = bat ]; then
  # the Run dialog truncates long lines: stage a batch file on the scratch disk (before the guest mounts it)
  sed 's/\r$//; s/$/\r/' "${1:?batch file}" > "$OUT/RUN.BAT"
  mcopy -o -i "$SCRATCH@@1048576" "$OUT/RUN.BAT" ::/RUN.BAT
fi
SOCK="$OUT/qmp.sock"; rm -f "$SOCK"
ACCEL=(-cpu pentium3)
[ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu host)
LOG="$OUT/qemu-$MODE.log"
CD2=()
CD1="$ISO"
if [ -n "${GAME_ISO:-}" ]; then CD1="$GAME_ISO"; CD2=(-drive "file=$ISO,media=cdrom,if=ide,index=3,readonly=on"); fi
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.so.0}"
"$ROOT/build/qemu/qemu-system-i386" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc -m 512 \
  -hda "$IMG" -hdb "$SCRATCH" -cdrom "$CD1" "${CD2[@]}" -vga none -device "d3dpt-vga,ddflags=${DDFLAGS:-0}" \
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
  d3d7)
    run 'D:\DRIVER\D3D7TEST.EXE 640 480 32 300 & copy d3d7test.log E:\d3d7.log & copy d3d7test.bmp E:\d3d7.bmp'
    sleep 8; Q screendump "$OUT/d3d7-fullscreen.png"
    sleep 30
    finish
    pull d3d7.log
    mcopy -n -i "$SCRATCH@@1048576" ::/d3d7.bmp "$OUT/d3d7.bmp" 2>/dev/null || true
    # the same scene through the executor without a guest (tools/d3dpt-dp2-test.cpp): the frames must agree
    if [ -f "$OUT/d3d7.bmp" ] && [ -x "$ROOT/build/d3dpt-dp2-test" ]; then
      ( cd "$ROOT" && D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}" build/d3dpt-dp2-test "$OUT/d3d7-host.bmp" >"$OUT/d3d7-host.log" 2>&1 ) || true
      python3 "$ROOT/tools/bmpdiff.py" "$OUT/d3d7-host.bmp" "$OUT/d3d7.bmp" --tolerance 2 --max-over 0 -o "$OUT/d3d7-diff.bmp" && echo "-- d3d7: guest frame == host frame" || echo "-- d3d7: FRAMES DIFFER ($OUT/d3d7-diff.bmp)"
    fi ;;
  cmd|bat)
    if [ "$MODE" = bat ]; then run 'E:\RUN.BAT'; else run "${1:?guest command line}"; fi
    if [ -n "${SHOTS:-}" ]; then
      for i in $(seq -f '%02g' 1 "$SHOTS"); do
        sleep "${SHOT_EVERY:-5}"
        # SHOT_KEYS="12:esc,14:ret": press the key (qmpc.py names) just before that screendump
        SK="${SHOT_KEYS:-}"; for k in ${SK//,/ }; do [ "${k%%:*}" = "${i#0}" ] && { Q keys "${k#*:}" || true; sleep 1; }; done
        Q screendump "$OUT/cmd-$i.png" || true
      done
    else
      sleep "${CMD_WAIT:-30}"
    fi
    finish ;;
  *) echo "unknown mode $MODE"; kill $QPID; exit 1 ;;
esac
