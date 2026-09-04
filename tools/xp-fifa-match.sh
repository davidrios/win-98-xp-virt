#!/usr/bin/env bash
# xp-fifa-match.sh — FIFA 2000 into a real match on the d3dpt-vga HAL,
# headless, and a keyboard test there (doc 15 "FIFA 2000 on the HAL").
#
#   tools/xp-fifa-match.sh kvm|tcg <image.qcow2> [outdir]
#
# Boots bare QEMU with the FIFA disc as D: and the driver ISO as F:, stages
# tools/xp-fifa2000.bat (+ D3DPT\DINPUT.DLL when built) on the E: scratch
# disk, runs it, then drives the game over QMP: Esc skips the intro, the
# title is detected as the first static frame, AMISTOSO's ball icon and the
# arrows are long-held mouse clicks (the menus ignore 100 ms clicks), Left
# picks the side, Avançar starts the match, which kicks off by itself. In the
# match: F2 (tower camera), F1, Right held 2 s, Esc (pause menu), Esc, F12
# (exit dialog) as 100 ms taps, a screendump after each — without the DINPUT
# shim the match ignores all of them; with it every one shows. Outputs in
# outdir (default build/xp-driver-test/fifa-match-<mode>): the screendumps,
# qemu.log (the executor's frames/s lines), and dinput_log.txt pulled from
# the image afterwards with 7z (the game never exits on its own).
# Env: FIFA_ISO (default /mnt/data2/david/Downloads/oldstuff/FIFA2000.ISO).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:?kvm|tcg}"; IMG="${2:?image.qcow2}"; OUT="${3:-$ROOT/build/xp-driver-test/fifa-match-$MODE}"
FIFA_ISO="${FIFA_ISO:-/mnt/data2/david/Downloads/oldstuff/FIFA2000.ISO}"
ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
[ -f "$ISO" ] || { echo "no $ISO: run guest-tools/build-driver.sh"; exit 1; }
mkdir -p "$OUT"
SCRATCH="$OUT/scratch.img"
if [ ! -f "$SCRATCH" ]; then
  truncate -s 64M "$SCRATCH"
  printf 'label: dos\nstart=2048, type=0c\n' | sfdisk -q "$SCRATCH"
  mkfs.fat -F 32 --offset 2048 "$SCRATCH" >/dev/null
fi
sed 's/\r$//; s/$/\r/' "$ROOT/tools/xp-fifa2000.bat" > "$OUT/RUN.BAT"
mcopy -o -i "$SCRATCH@@1048576" "$OUT/RUN.BAT" ::/RUN.BAT
SHIM="$ROOT/guest-tools/out/iso/D3DPT/dinput.dll"
[ -f "$SHIM" ] && mcopy -o -i "$SCRATCH@@1048576" "$SHIM" ::/DINPUT.DLL && echo "DINPUT.DLL staged (the keyboard fix)"
SOCK="/tmp/xp-fifa-$$.sock"; rm -f "$SOCK"      # short: a Unix socket path is limited to ~100 chars
trap 'rm -f "$SOCK"' EXIT
if [ "$MODE" = kvm ]; then ACCEL=(-accel kvm -cpu host); SLOW=1; else ACCEL=(-cpu pentium3); SLOW=3; fi
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.so}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.so.0}"
"$ROOT/build/qemu/qemu-system-i386" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc -m 512 \
  -hda "$IMG" -hdb "$SCRATCH" -cdrom "$FIFA_ISO" -drive "file=$ISO,media=cdrom,if=ide,index=3,readonly=on" \
  -vga none -device d3dpt-vga -net none -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial none -monitor none > "$OUT/qemu.log" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
ev() { Q json "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$1]}}" >/dev/null; }
key_hold() { ev "{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}"; sleep "$2"; ev "{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}"; }
lclick() {  # x y (of 640x480) wait name: a 1.5 s left click, the menus ignore short ones
  ev "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(( $1 * 32767 / 640 ))}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(( $2 * 32767 / 480 ))}}"
  sleep 0.5; ev '{"type":"btn","data":{"down":true,"button":"left"}}'; sleep 1.5; ev '{"type":"btn","data":{"down":false,"button":"left"}}'
  sleep "$3"; Q screendump "$OUT/$4.png"
}
t0=$(date +%s); T() { echo "[$(( $(date +%s) - t0 ))s] $*"; }
sleep $(( 45 * SLOW + 5 )); T "desktop"
n0=$(grep -c "linear mode on (640x480" "$OUT/qemu.log")
Q keys esc; sleep 1; Q keys meta_l+r; sleep 2; Q type 'cmd /k E:\RUN.BAT'; Q keys ret
until [ "$(grep -c "linear mode on (640x480" "$OUT/qemu.log")" -gt "$n0" ]; do sleep 3; done
T "game up"; sleep $(( 5 * SLOW )); Q keys esc          # skips the intro video
prev=""
for i in $(seq 1 40); do                                  # the title screen is the first static frame
  sleep 6; cur=$(Q screendump "$OUT/title.png" >/dev/null; md5sum < "$OUT/title.png")
  [ "$cur" = "$prev" ] && break; prev="$cur"
done
T "title (after $i polls)"
Q keys ret; sleep $(( 8 * SLOW )); Q screendump "$OUT/menu.png"
lclick 170 287 $(( 8 * SLOW )) teams; T "team select"
lclick 600 440 $(( 8 * SLOW )) side; T "side select"
Q keys left; sleep $(( 4 * SLOW )); Q screendump "$OUT/side-left.png"
lclick 600 440 80 match; T "match (80 s after Avançar: it kicks off by itself)"
grep "frames/s" "$OUT/qemu.log" | tail -1 | sed 's/qemu-system-i386: info: //'
Q screendump "$OUT/m0.png"
Q keys f2; sleep 4; Q screendump "$OUT/m1-f2.png"; T "after F2 (tower camera)"
Q keys f1; sleep 4; Q screendump "$OUT/m2-f1.png"; T "after F1 (TV camera)"
key_hold right 2; Q screendump "$OUT/m3-right.png"; T "after Right held 2 s"
Q keys esc; sleep 4; Q screendump "$OUT/m4-esc.png"; T "after Esc (pause menu)"
Q keys esc; sleep 4; Q screendump "$OUT/m5-esc.png"; T "after Esc again"
Q keys f12; sleep 8; Q screendump "$OUT/m6-f12.png"; T "after F12 (exit dialog)"
grep "frames/s" "$OUT/qemu.log" | tail -1 | sed 's/qemu-system-i386: info: //'
Q json '{"execute":"system_powerdown"}' >/dev/null
sleep 40; kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
if command -v 7z >/dev/null; then
  7z e -y -so "$IMG" "Arquivos de programas/EA SPORTS/FIFA 2000/dinput_log.txt" 2>/dev/null | tr -d '\r' > "$OUT/dinput_log.txt"
  echo "---- dinput_log.txt: keys the game's DirectInput state reported in the match, and what Windows saw"
  awk '/shim loaded/{n++} {if (n) print}' "$OUT/dinput_log.txt" | grep -E "state DIK|async: VK|set from GetAsyncKeyState" | tail -20
fi
T "done: screendumps in $OUT"
