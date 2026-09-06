#!/usr/bin/env bash
# glide-guest-test.sh — GLIDETEST.EXE in a real Win98 guest, in the player,
# headless. The whole Glide chain end to end (doc 12 §5):
#
#   the guest's GLIDE2X.DLL -> the MMIO FIFO -> hw/3dfx's dispatcher ->
#   our libglide2x on the host -> the embed backend's window-less context
#   -> the player's shader chain
#
#   tools/glide-guest-test.sh ~/vms/win98.qcow2
#
# **It must be the player, not qemu-system-i386.** A bare QEMU registers no
# 3D UI provider, so `glide_host_ops` returns NULL, the wrapper is left with
# its own windowing and finds no context: grSstWinOpen fails and the guest
# falls back to software. That is the design (patch 33), not a bug — the
# provider is the embed library's, and only the player has one.
#
# The proof is the guest's own line, `glidetest: N cases, 0 failed`, on
# COM1: GLIDETEST reads its pixels back through grLfbLock rather than
# trusting the host to look at them — a host that never drew the scene
# cannot make those pixels up.
#
# Needs a guest image, so it is run by hand and never from scripts/test.sh.
# The image is never written: everything goes to a qcow2 overlay under
# build/glide-test. Win98 runs emulated by decision (CLAUDE.md: under KVM
# this image's Explorer dies at startup and there is no Start menu to type
# into). Even so this image boots in well under a minute under TCG, so the
# whole run is about four minutes; the waits below are that plus slack.
#
# Env: OUT=dir, BOOT_WAIT=s, WARMUP_WAIT=s, NO_WARMUP=1, RES=7 (the Glide
# resolution, glidewnd.c's table), REUSE=1 (keep the last overlay, which
# already has the wrapper installed and Win98 settled: halves the run),
# DUMP_SEQ=n (the player writes its own shaded frame #n to frame.png and
# **ends the run there** — the dump exits the player; the guest's own
# readback is the evidence, this is only for eyes).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?image.qcow2}"
OUT="${OUT:-$ROOT/build/glide-test}"; mkdir -p "$OUT"
OVL="$OUT/overlay.qcow2"
FLOPPY="$OUT/run.img"
LOG="$OUT/serial.log"
QLOG="$OUT/player.log"
# an AF_UNIX path is capped at 108 bytes and a build/ path under a deep
# checkout is already close: keep the socket short and outside the tree
SOCK="${SOCK:-/tmp/2ks-glide.sock}"
PLAYER="$ROOT/target/release/player"
QEMU="$ROOT/build/qemu/qemu-system-i386"
WRAPPER="$ROOT/build/glide/libglide2x.so"
ISO="$(ls -t "$ROOT"/guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)"
EXE="$OUT/GLIDETEST.EXE"

[ -x "$PLAYER" ] || { echo "no $PLAYER: cargo build --release"; exit 1; }
[ -f "$WRAPPER" ] || { echo "no $WRAPPER: scripts/build-glide.sh"; exit 1; }
[ -f "$ISO" ] || { echo "no guest-tools ISO: guest-tools/build-wrappers.sh"; exit 1; }

# GLIDETEST.EXE goes on the floppy rather than the ISO, so this runs against
# whatever guest-tools ISO is already built: only SETUP.EXE is needed off the
# disc, to install GLIDE2X.DLL into the guest.
#
# The two flag sets are build-wrappers.sh's, and the check after is why they
# are spelled out here rather than assumed: modern mingw-w64 defaults to the
# UCRT, which Win9x has none of, and the failure is a "A required .DLL file,
# API-MS-WIN-CRT-CONVERT-L1-1-0.DLL, was not found" box in the guest with an
# empty serial log to look at. -march=pentium3 is the guest CPU floor.
echo "==> building GLIDETEST.EXE"
i686-w64-mingw32-gcc -O2 -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -o "$EXE" "$ROOT/guest-tools/src/glidetest.c" \
  -I"$ROOT/third_party/openglide" \
  -L"$ROOT/third_party/qemu-3dfx/wrappers/3dfx/build" -lglide2x -luser32 \
  -march=pentium3 -mtune=generic
if objdump -p "$EXE" | grep -q 'api-ms-win-crt'; then
  echo "ERROR: $EXE links against the UCRT (not loadable on Win9x)"; exit 1
fi

# The CD's drive letter differs per image and COMMAND.COM cannot be relied
# on to set a variable inside a FOR, so every command is written once per
# candidate letter behind `if exist` (the same trick setup-guest-test.sh uses).
{
  echo '@echo off'
  if [ -z "${REUSE:-}" ]; then
    echo 'echo ==== installing the Glide wrapper > COM1'
    for d in D E F G; do
      printf 'if exist %s:\\SETUP.EXE %s:\\SETUP.EXE /ALL > COM1\n' "$d" "$d"
    done
  fi
  echo 'echo ==== glidetest > COM1'
  printf 'A:\\GLIDETEST.EXE -res %s > COM1\n' "${RES:-7}"
  echo 'echo GLIDEDONE > COM1'
} > "$OUT/RUN.BAT"
sed -i 's/\r$//; s/$/\r/' "$OUT/RUN.BAT"
rm -f "$FLOPPY"
mkfs.fat -C -F 12 "$FLOPPY" 1440 >/dev/null
mcopy -o -i "$FLOPPY" "$OUT/RUN.BAT" ::/RUN.BAT
mcopy -o -i "$FLOPPY" "$EXE" ::/GLIDETEST.EXE

# REUSE=1 keeps the overlay from the last run — the Glide wrapper is already
# installed in it and Win98 has already settled, which halves the run
# while iterating.
if [ -z "${REUSE:-}" ] || [ ! -f "$OVL" ]; then
  rm -f "$OVL"
  "$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$IMG" -F qcow2 "$OVL"
else
  echo "==> reusing $OVL"
fi
rm -f "$SOCK" "$LOG"

# doc 06's Win98 machine, emulated (see the header)
HW=(-cpu pentium3 -machine pc -m 256 -vga cirrus
    -netdev user,id=n0 -device pcnet,netdev=n0
    -audiodev none,id=a0 -device sb16,audiodev=a0)

# Win98 re-detects its hardware on the first boot of a fresh overlay and
# that boot regularly takes Explorer down with it — no taskbar, no Start
# menu, no way in. Burn one boot first and shut it down over ACPI, which
# needs no shell; the second boot comes up settled. Bare QEMU is fine for
# this one: nothing 3D happens.
if [ -z "${NO_WARMUP:-}" ]; then
  echo "==> warm-up boot (${WARMUP_WAIT:-90}s)"
  "$QEMU" -L "$ROOT/qemu/pc-bios" "${HW[@]}" -hda "$OVL" -boot c \
    -display none -qmp "unix:$SOCK,server,nowait" -monitor none \
    > "$OUT/warmup.log" 2>&1 &
  WPID=$!
  sleep "${WARMUP_WAIT:-90}"
  python3 "$ROOT/tools/qmpc.py" "$SOCK" json '{"execute":"system_powerdown"}' >/dev/null 2>&1 || true
  for _ in $(seq 1 120); do sleep 1; kill -0 $WPID 2>/dev/null || break; done
  kill $WPID 2>/dev/null || true; wait $WPID 2>/dev/null || true
  rm -f "$SOCK"
fi

echo "==> the player, with the guest-tools ISO and the run floppy"
export QEMU_GLIDE_LIB="$WRAPPER" GLIDE_HOST_LOG="$OUT/wrapper.log"
# the shaded frame, for eyes only: the dump ends the player, so it is off
# unless asked for (and then the run has no verdict, by construction)
if [ -n "${DUMP_SEQ:-}" ]; then
  export PLAYER_DUMP_OUT="$OUT/frame.png" PLAYER_DUMP_SEQ="$DUMP_SEQ"
fi
"$PLAYER" -- -L "$ROOT/qemu/pc-bios" "${HW[@]}" \
  -hda "$OVL" -fda "$FLOPPY" -boot c -cdrom "$ISO" \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" \
  > "$QLOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }

sleep "${BOOT_WAIT:-60}"
Q screendump "$OUT/boot.png" || true
# Typing into the Run dialog is the only way in and it can miss (the shell
# may still be starting, or a message box may be in front of it), so each
# attempt dismisses whatever is there, opens Run, types, and waits to see
# whether any output actually arrived.
for attempt in $(seq 1 "${ATTEMPTS:-4}"); do
  Q keys ret || true; sleep 3
  Q keys esc || true; sleep 1
  Q keys ctrl+esc || true; sleep 3; Q keys r || true
  sleep 3
  Q screendump "$OUT/run$attempt.png" || true
  Q type 'command /c A:\RUN.BAT' || true; Q keys ret || true
  for i in $(seq 1 30); do sleep 1; [ -s "$LOG" ] && break; done
  [ -s "$LOG" ] && break
  echo "attempt $attempt: nothing on the serial line yet, retrying"
  sleep 20
done
for i in $(seq 1 "${WAIT_SECS:-420}"); do
  sleep 1
  grep -q GLIDEDONE "$LOG" 2>/dev/null && break
done
Q screendump "$OUT/end.png" || true
# a Win98 run ends with a Start-menu shutdown, never a kill (CLAUDE.md)
Q keys ctrl+esc || true; sleep 2; Q keys u || true; sleep 2; Q keys ret || true
for i in $(seq 1 90); do sleep 1; kill -0 $QPID 2>/dev/null || break; done
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true
rm -f "$SOCK"

echo
echo "==== the guest's serial output ===================================="
sed 's/\r$//' "$LOG" 2>/dev/null || echo "(nothing)"
echo "==================================================================="
echo "player log:  $QLOG      (grep glidept: for the wrapper it loaded)"
echo "wrapper log: $OUT/wrapper.log"
echo "screendumps: $OUT/*.png${DUMP_SEQ:+, frame $OUT/frame.png}"
grep -a "glidept:" "$QLOG" 2>/dev/null | head -5 || true

if grep -qa "glidetest: .* 0 failed" "$LOG" 2>/dev/null; then
  echo "PASS $(grep -a 'glidetest: .* cases' "$LOG" | tail -1 | tr -d '\r')"
  exit 0
fi
echo "FAIL — no clean 'glidetest: N cases, 0 failed' on the serial line"
exit 1
