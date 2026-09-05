#!/usr/bin/env bash
# cdshelf-guest-test.sh — CDSHELF.EXE against a real Windows guest (doc 07's
# disc shelf, patch 52, protocol cdshelf/cdshelf_proto.h). The DOS build is
# covered by tools/atapi-guest-test.py, which needs no guest image; this is
# the Windows half, and it needs one, so it is run by hand rather than from
# scripts/test.sh.
#
#   tools/cdshelf-guest-test.sh ~/vms/winxp.qcow2 xp
#   tools/cdshelf-guest-test.sh ~/vms/win98.qcow2 win98
#
# The machine boots with an EMPTY tray and a shelf of two discs: a generated
# ISO with two files on it, and a path that does not exist. The guest then
# lists the shelf, loads the ISO, reads its files through Windows' own file
# system driver (`dir` and `type` — that is the proof the tray really
# changed, not a status byte), refuses the missing one, ejects, and lists
# again. Output comes back over COM1; PASS/FAIL per check at the end.
#
# The image is never written: everything goes to a qcow2 overlay under
# build/cdshelf-test. CDSHELF.EXE rides in on a floppy so its drive is A: on
# both families, and the guest is driven through the Run dialog over QMP.
#
# Env: OUT=dir (default build/cdshelf-test), BOOT_WAIT=s, NO_KVM=1,
# KEEP=1 (leave the VM running on failure).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?image.qcow2}"; FAMILY="${2:-xp}"
OUT="${OUT:-$ROOT/build/cdshelf-test}"; mkdir -p "$OUT"
OVL="$OUT/overlay.qcow2"
FLOPPY="$OUT/tools.img"
SHELF="$OUT/shelf.txt"
ISO="$OUT/shelfdisc.iso"
LOG="$OUT/serial-$FAMILY.log"
QLOG="$OUT/qemu-$FAMILY.log"
SOCK="$OUT/q.sock"
QEMU="$ROOT/build/qemu/qemu-system-i386"
[ -x "$QEMU" ] || { echo "no $QEMU: build QEMU first"; exit 1; }

# the disc the guest will load, with two files it can read back
rm -rf "$OUT/isoroot" && mkdir -p "$OUT/isoroot"
printf 'the disc shelf works\r\n' > "$OUT/isoroot/HELLO.TXT"
printf 'second file\r\n' > "$OUT/isoroot/DISC2.TXT"
if command -v xorriso >/dev/null; then
  xorriso -as mkisofs -o "$ISO" -V SHELFTEST -J -r "$OUT/isoroot" >/dev/null 2>&1
elif command -v genisoimage >/dev/null; then
  genisoimage -o "$ISO" -V SHELFTEST -J -r "$OUT/isoroot" >/dev/null 2>&1
else
  echo "need xorriso or genisoimage"; exit 1
fi
{ printf 'Shelf test disc\t%s\n' "$ISO"
  printf 'Not on the host\t%s\n' "$OUT/no-such-disc.iso"; } > "$SHELF"
rm -f "$OUT/no-such-disc.iso"

# -mwindows because the program's normal face is a window; its command-line
# verbs (the ones below) still write to a redirected stdout, which is how
# their output gets out of the guest over COM1.
i686-w64-mingw32-gcc -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -march=pentium3 -mwindows -I"$ROOT/cdshelf" -o "$OUT/CDSHELF.EXE" \
  "$ROOT/guest-tools/src/cdshelf.c"

# One batch for both families: COMMAND.COM (98) and CMD.EXE (XP) both take
# it, and every line's output goes to COM1, which needs no writable disk in
# the guest. The CD-ROM's own letter differs per image, so the disc is read
# from both D: and E: and one of the two prints a "path not found" — that
# noise is cheaper than teaching this script every image's letters.
cat > "$OUT/RUN.BAT" <<'BAT'
@echo off
A:
cd \
echo ==== list, empty tray > COM1
CDSHELF.EXE list > COM1
echo ==== load 0 > COM1
CDSHELF.EXE 0 > COM1
dir /b D:\ > COM1
dir /b E:\ > COM1
type D:\HELLO.TXT > COM1
type E:\HELLO.TXT > COM1
echo ==== load 1, missing on the host > COM1
CDSHELF.EXE 1 > COM1
echo ==== eject > COM1
CDSHELF.EXE E > COM1
CDSHELF.EXE list > COM1
echo CDSHELFDONE > COM1
BAT
sed -i 's/\r$//; s/$/\r/' "$OUT/RUN.BAT"
rm -f "$FLOPPY"
mkfs.fat -C -F 12 "$FLOPPY" 1440 >/dev/null
mcopy -o -i "$FLOPPY" "$OUT/RUN.BAT" ::/RUN.BAT
mcopy -o -i "$FLOPPY" "$OUT/CDSHELF.EXE" ::/CDSHELF.EXE

# never write the image itself
rm -f "$OVL"
"$ROOT/build/qemu/qemu-img" create -q -f qcow2 -b "$IMG" -F qcow2 "$OVL"
rm -f "$SOCK" "$LOG"

ACCEL=(-cpu pentium3)
[ -e /dev/kvm ] && [ -z "${NO_KVM:-}" ] && ACCEL=(-accel kvm -cpu pentium3)
if [ "$FAMILY" = win98 ]; then
  # doc 06's Win98 machine, so Windows does not re-detect hardware on the
  # first boot of the overlay (which can take Explorer down with it)
  HW=(-m 256 -vga cirrus -netdev user,id=n0 -device pcnet,netdev=n0
      -audiodev none,id=a0 -device sb16,audiodev=a0)
  SHELL_CMD='command /c A:\RUN.BAT'
else
  HW=(-m 512 -vga std -net none)
  SHELL_CMD='cmd /k A:\RUN.BAT'
fi
"$QEMU" -L "$ROOT/qemu/pc-bios" "${ACCEL[@]}" -machine pc "${HW[@]}" \
  -hda "$OVL" -fda "$FLOPPY" -boot c \
  -drive if=none,id=cd0,media=cdrom \
  -device "ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,shelf=$SHELF" \
  -usb -device usb-tablet -display none \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" -monitor none > "$QLOG" 2>&1 &
QPID=$!
Q() { python3 "$ROOT/tools/qmpc.py" "$SOCK" "$@"; }

sleep "${BOOT_WAIT:-$([ "$FAMILY" = win98 ] && echo 120 || echo 60)}"
Q screendump "$OUT/$FAMILY-boot.png" || true
# Typing into the Run dialog is the only way in, and it can miss: the shell
# may still be starting, or (Win98, first boot of a fresh overlay) an
# "illegal operation" box may be in front of it. So each attempt dismisses
# whatever is there, opens Run, types, and then waits to see whether any
# output actually arrived before trying again. The screendump of every
# attempt is kept — it is the only way to see what the guest was showing.
for attempt in $(seq 1 "${ATTEMPTS:-4}"); do
  Q keys ret || true; sleep 3          # close a message box, if any
  Q keys esc || true; sleep 1
  if [ "$FAMILY" = win98 ]; then Q keys ctrl+esc || true; sleep 3; Q keys r || true
  else Q keys meta_l+r || true; fi
  sleep 3
  Q screendump "$OUT/$FAMILY-run$attempt.png" || true
  Q type "$SHELL_CMD" || true; Q keys ret || true
  for i in $(seq 1 30); do
    sleep 1
    [ -s "$LOG" ] && break
  done
  [ -s "$LOG" ] && break
  echo "attempt $attempt: nothing on the serial line yet, retrying"
  sleep 20
done
for i in $(seq 1 "${WAIT_SECS:-150}"); do
  sleep 1
  grep -q CDSHELFDONE "$LOG" 2>/dev/null && break
done
Q screendump "$OUT/$FAMILY-end.png" || true
if [ "$FAMILY" = win98 ]; then
  # a Win98 run ends with a Start-menu shutdown, never a kill (CLAUDE.md)
  Q keys ctrl+esc || true; sleep 2; Q keys u || true; sleep 2; Q keys ret || true
  for i in $(seq 1 90); do sleep 1; kill -0 $QPID 2>/dev/null || break; done
else
  Q json '{"execute":"system_powerdown"}' >/dev/null || true
  for i in $(seq 1 60); do sleep 1; kill -0 $QPID 2>/dev/null || break; done
fi
if kill -0 $QPID 2>/dev/null; then
  [ -n "${KEEP:-}" ] || { kill $QPID 2>/dev/null || true; }
fi
wait $QPID 2>/dev/null || true

echo "---- $LOG"
cat "$LOG" || true
fails=0
want() {  # a line that must be in the output
  if grep -qF "$1" "$LOG"; then echo "PASS  $2"; else echo "FAIL  $2 (missing: $1)"; fails=$((fails + 1)); fi
}
echo "----"
want "CDSHELFDONE" "the batch ran to the end"
want "Shelf test disc" "the shelf was listed"
want "[missing on the host]" "the unreachable disc is flagged"
want "loading slot 0: Shelf test disc" "the load names the disc"
want "the disc is in the drive." "the drive reported the new medium"
want "HELLO.TXT" "Windows read the loaded disc's directory"
want "the disc shelf works" "Windows read a file off the loaded disc"
want "the host cannot reach that disc image" "the missing disc was refused"
want "the drive is empty." "the eject was accepted"
if [ "$fails" = 0 ]; then
  echo "cdshelf guest test ($FAMILY): PASS"
else
  echo "cdshelf guest test ($FAMILY): FAIL ($fails checks), see $OUT/$FAMILY-*.png and $QLOG"
  exit 1
fi
