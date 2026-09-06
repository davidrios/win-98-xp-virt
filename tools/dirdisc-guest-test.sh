#!/usr/bin/env bash
# dirdisc-guest-test.sh — a host folder in a guest's CD-ROM drive (M5g,
# docs/tracks/m5-dirdisc.md), on the families xp-cdimage-test.sh does not
# cover:
#
#   tools/dirdisc-guest-test.sh ~/vms/win98.qcow2 win98
#   tools/dirdisc-guest-test.sh ~/vms/winxp.qcow2 xp
#
# The machine boots with `-cdrom isodir:<dir>` — no image anywhere — and a
# floppy carrying RUN.BAT, which lists the disc and reads files off it
# through the guest's own file system driver, over COM1. That is the
# proof: the names in the listing and the bytes of the files are what
# libdisc generated from the tree as the guest asked for them.
#
# The folder is deliberately awkward in the ways a user's own folder is:
# a long name, a space in a directory name, a file of exactly one sector
# and one a byte over, an empty file, and a nested directory.
#
# The image is never written: everything goes to a qcow2 overlay under
# build/dirdisc-guest. Local only (needs a guest image), so not in
# scripts/test.sh. Env: OUT=dir, BOOT_WAIT=s, NO_KVM=1, KEEP=1.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?image.qcow2}"; FAMILY="${2:-win98}"
OUT="${OUT:-$ROOT/build/dirdisc-guest}"; mkdir -p "$OUT"
OVL="$OUT/overlay.qcow2"; FLOPPY="$OUT/tools.img"; SRC="$OUT/shared"
LOG="$OUT/serial-$FAMILY.log"; QLOG="$OUT/qemu-$FAMILY.log"; SOCK="$OUT/q.sock"
QEMU="$ROOT/build/qemu/qemu-system-i386"
[ -x "$QEMU" ] || { echo "no $QEMU: build QEMU first"; exit 1; }

# The shared folder itself.
rm -rf "$SRC"; mkdir -p "$SRC/Patch Notes"
printf 'a folder is a disc\r\n' > "$SRC/FOLDER.TXT"
printf 'long names survive\r\n' > "$SRC/Patch Notes/Read Me First.txt"
: > "$SRC/EMPTY.BIN"
head -c 2048 /dev/zero | tr '\0' 'A' > "$SRC/EXACT.BIN"
head -c 2049 /dev/zero | tr '\0' 'B' > "$SRC/ODD.BIN"

# One batch for both families: COMMAND.COM (98) and CMD.EXE (XP) both take
# it, and every line goes to COM1, which needs no writable disk in the
# guest. The CD's letter differs per image, so both D: and E: are tried
# and one of them prints a "not found" — cheaper than teaching this script
# every image's drive letters.
cat > "$OUT/RUN.BAT" <<'BAT'
@echo off
echo ==== the folder as a disc > COM1
dir /b D:\ > COM1
dir /b E:\ > COM1
type D:\FOLDER.TXT > COM1
type E:\FOLDER.TXT > COM1
dir /b "D:\Patch Notes" > COM1
dir /b "E:\Patch Notes" > COM1
type "D:\Patch Notes\Read Me First.txt" > COM1
type "E:\Patch Notes\Read Me First.txt" > COM1
echo DIRDISCDONE > COM1
BAT
python3 - "$OUT/RUN.BAT" <<'CRLF'
import sys
p = sys.argv[1]
text = open(p, newline="").read().replace("\r\n", "\n").replace("\n", "\r\n")
open(p, "w", newline="").write(text)
CRLF
rm -f "$FLOPPY"
if command -v mkfs.fat >/dev/null; then
  mkfs.fat -C -F 12 "$FLOPPY" 1440 >/dev/null
else
  mformat -C -f 1440 -i "$FLOPPY" ::
fi
mcopy -o -i "$FLOPPY" "$OUT/RUN.BAT" ::/RUN.BAT

rm -f "$OVL"; "$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$IMG" -F qcow2 "$OVL"
rm -f "$SOCK" "$LOG"
ACCEL=(-cpu pentium3)
[ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu pentium3)
if [ "$FAMILY" = win98 ]; then
  HW=(-m 256 -vga cirrus -netdev user,id=n0 -device pcnet,netdev=n0)
  SHELL_CMD='command /c A:\RUN.BAT'
else
  HW=(-m 512 -vga std -net none)
  SHELL_CMD='cmd /k A:\RUN.BAT'
fi
"$QEMU" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc "${HW[@]}" \
  -hda "$OVL" -fda "$FLOPPY" -boot c \
  -cdrom "isodir:$SRC" \
  -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" -monitor none > "$QLOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }

sleep "${BOOT_WAIT:-$([ "$FAMILY" = win98 ] && echo 120 || echo 45)}"
Q screendump "$OUT/$FAMILY-boot.png" || true
for attempt in $(seq 1 "${ATTEMPTS:-4}"); do
  Q keys ret || true; sleep 3
  Q keys esc || true; sleep 1
  if [ "$FAMILY" = win98 ]; then Q keys ctrl+esc || true; sleep 3; Q keys r || true
  else Q keys meta_l+r || true; fi
  sleep 3
  Q screendump "$OUT/$FAMILY-run$attempt.png" || true
  Q type "$SHELL_CMD" || true; Q keys ret || true
  for i in $(seq 1 30); do sleep 1; [ -s "$LOG" ] && break; done
  [ -s "$LOG" ] && break
  echo "attempt $attempt: nothing on the serial line yet, retrying"
  sleep 20
done
for i in $(seq 1 "${WAIT_SECS:-120}"); do sleep 1; grep -q DIRDISCDONE "$LOG" 2>/dev/null && break; done
Q screendump "$OUT/$FAMILY-end.png" || true
if [ "$FAMILY" = win98 ]; then
  # a Win98 run ends with a Start-menu shutdown, never a kill (CLAUDE.md)
  Q keys ctrl+esc || true; sleep 2; Q keys u || true; sleep 2; Q keys ret || true
  for i in $(seq 1 90); do sleep 1; kill -0 $QPID 2>/dev/null || break; done
else
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  for i in $(seq 1 60); do sleep 1; kill -0 $QPID 2>/dev/null || break; done
fi
kill -0 $QPID 2>/dev/null && { [ -n "${KEEP:-}" ] || kill $QPID 2>/dev/null || true; }
wait $QPID 2>/dev/null || true

echo "---- $LOG"; cat "$LOG" || true
fails=0
want() { if grep -qF "$1" "$LOG"; then echo "PASS  $2"; else echo "FAIL  $2 (missing: $1)"; fails=$((fails + 1)); fi; }
echo "----"
want "DIRDISCDONE" "the batch ran to the end"
want "FOLDER.TXT" "the guest listed the generated volume"
want "a folder is a disc" "the guest read a file out of the folder"
want "Read Me First.txt" "long names survived (Joliet)"
want "long names survive" "the guest read a file from a directory with a space in its name"
want "EMPTY.BIN" "the empty file is in the listing"
if [ "$fails" = 0 ]; then
  echo "dirdisc guest test ($FAMILY): PASS"
else
  echo "dirdisc guest test ($FAMILY): FAIL ($fails checks), see $OUT/$FAMILY-*.png and $QLOG"
  exit 1
fi
