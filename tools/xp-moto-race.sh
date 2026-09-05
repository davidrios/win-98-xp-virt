#!/usr/bin/env bash
# xp-moto-race.sh — Moto Racer 1997 into a practice race, headless, and its
# frame rate from outside (M9 track: the before/after number for the
# software renderer under TCG that the vCPU's % split cannot give).
#
#   tools/xp-moto-race.sh <image.qcow2> <name> [qemu-system-i386]
#
# Boots through tools/tcg-profile.sh (KEEP=1, the player's disc layout:
# CDS=MOTO_RACER.mds:FIFA2000.ISO, the M7 adapter), lets the attract-mode demo
# start, quits it (Esc, "Quit demo"), clicks Start on the title, accepts the
# name, Play Solo -> Practice -> Time Attack off -> Continue (Speed Bay, 3 laps)
# -> Start, waits
# for the load and the countdown, then holds the throttle while
# tools/tcg-fps.py counts distinct VGA frames for FPS seconds (15).
# Screendumps of every step and `fps.txt` land in build/tcg-profile/<name>/.
# Env: MOTO (the .mds), FIFA (the .iso), FPS, plus the runner's.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?image.qcow2}"; NAME="${2:?name}"; BIN="${3:-$ROOT/build/qemu/qemu-system-i386}"
MOTO="${MOTO:-$HOME/vms/Moto.Racer.1997.DSI.CD/MOTO_RACER.mds}"; FIFA="${FIFA:-$HOME/vms/FIFA2000.ISO}"
OUT="$ROOT/build/tcg-profile/$NAME"; mkdir -p "$OUT"
KEEP=1 QEMU_BIN="$BIN" CDS="$MOTO:$FIFA" VGA=d3dpt MEM=1024 BOOT_WAIT="${BOOT_WAIT:-120}" WARM=25 SECS=1 \
  "$ROOT/tools/tcg-profile.sh" "$IMG" "$NAME" 'cmd /k cd /d C:\Arquiv~1\MotoRacer & MOTO.EXE' > "$OUT/run.log" 2>&1
SOCK=$(grep -o '/tmp/tcgprof-[0-9]*.sock' "$OUT/run.log" | head -1)
QPID=$(grep -o 'pid [0-9]*' "$OUT/run.log" | head -1 | cut -d' ' -f2)
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }
ev() { Q json "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$1]}}" >/dev/null; }
click() {  # x y in the 640x480 frame, a 200 ms press
  ev "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(( $1 * 32767 / 640 ))}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(( $2 * 32767 / 480 ))}}"
  sleep 0.5; ev '{"type":"btn","data":{"down":true,"button":"left"}}'; sleep 0.2; ev '{"type":"btn","data":{"down":false,"button":"left"}}'
}
key() { ev "{\"type\":\"key\",\"data\":{\"down\":$2,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}"; }
shot() { Q screendump "$OUT/$1.png" >/dev/null || true; }
trap 'Q json "{\"execute\":\"system_powerdown\"}" >/dev/null 2>&1 || true; sleep 8; kill $QPID 2>/dev/null || true' EXIT
sleep 10; shot demo                       # the attract demo is running by now (the runner waited 25 s)
Q keys esc >/dev/null; sleep 2; Q keys down >/dev/null; sleep 1; Q keys ret >/dev/null; sleep 6; shot title
click 315 445; sleep 6; shot name         # Start
Q keys ret >/dev/null; sleep 6; shot menu # the name as it is
click 160 250; sleep 6; shot mode         # Play Solo
click 110 260; sleep 6; shot race         # Practice
click 450 430; sleep 3                    # Time Attack off: no time limit ending the race in the window
click 565 373; sleep 6; shot bike         # Continue (Speed Bay, 3 laps)
click 565 390; sleep 35; shot loaded      # Start; load + countdown
key up true
python3 "$ROOT/tools/tcg-fps.py" "$SOCK" "${FPS:-15}" | tee "$OUT/fps.txt"
shot racing; key up false
echo "== $NAME: $(cat "$OUT/fps.txt")  ($OUT)"
