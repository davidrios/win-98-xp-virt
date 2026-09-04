#!/usr/bin/env bash
# XP reads a cdimage disc through the real OS driver (doc 17 §6.3):
#
#   tools/xp-cdimage-test.sh <winxp.qcow2> <disc.cue|.ccd|.iso> <reference dir> [outdir]
#
# Boots the XP image read-only (snapshot=on) with the disc as the IDE CD-ROM
# (-cdrom: the probe path — a .cue/.ccd must reach the cdimage driver by
# itself) and a fresh FAT32 scratch disk (E:) carrying RUN.BAT, which copies
# the whole CD (xcopy /S /E, DMA through cdrom.sys) to E:\CD and lists it,
# then powers XP down over QMP and compares every file with <reference dir>
# (the files the image was made from, e.g. the ISO extracted with bsdtar).
# Prints PASS/FAIL, the copied file count, DIR output head and the QEMU log's
# cdimage lines. Exit 1 on any difference. Env: TEST_ACCEL=kvm|tcg,
# BOOT_TIMEOUT (300 s), TEST_KEEP=1 leaves XP running on failure.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
IMG="${1:?xp image}"; DISC="${2:?disc image}"; REF="${3:?reference dir}"; OUT="${4:-$ROOT/build/test/cdimage-xp}"
mkdir -p "$OUT"
for t in mkfs.fat sfdisk mcopy mmd; do command -v $t >/dev/null || { echo "needs $t"; exit 2; }; done
[ -x build/qemu/qemu-system-i386 ] || { echo "no build/qemu/qemu-system-i386"; exit 2; }
accel="${TEST_ACCEL:-}"; if [ -z "$accel" ]; then if [ -w /dev/kvm ]; then accel=kvm; else accel=tcg; fi; fi
cpu=(-cpu pentium3); [ "$accel" = kvm ] && cpu=(-cpu host)

scratch="$OUT/scratch.img"; rm -f "$scratch"; truncate -s 64M "$scratch"
printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$scratch" >/dev/null
mkfs.fat -F 32 --offset 2048 "$scratch" >/dev/null
fat="$scratch@@1048576"
printf '@echo off\r\nmkdir E:\\OUT\r\necho started > E:\\OUT\\STARTED.TXT\r\ndir D:\\ /S > E:\\OUT\\DIR.TXT\r\nxcopy D:\\ E:\\CD\\ /I /Y /S /E /H > E:\\OUT\\XCOPY.TXT\r\necho %%ERRORLEVEL%% > E:\\OUT\\XCOPYRC.TXT\r\necho done > E:\\OUT\\DONE.TXT\r\n' > "$OUT/RUN.BAT"
mcopy -i "$fat" "$OUT/RUN.BAT" ::/RUN.BAT

SOCK="$OUT/qmp.sock"; rm -f "$SOCK"; qlog="$OUT/qemu.log"
qmp() { python3 tools/qmpc.py "$SOCK" "$@" >/dev/null; }
echo "XP: $IMG (snapshot), disc: $DISC, accel: $accel"
build/qemu/qemu-system-i386 -L qemu/pc-bios -accel "$accel" "${cpu[@]}" -machine pc -m 512 \
  -drive "file=$IMG,if=ide,index=0,media=disk,snapshot=on" -drive "file=$scratch,format=raw,if=ide,index=1,media=disk" \
  -cdrom "$DISC" -vga cirrus -net none -usb -device usb-tablet -display none -serial none -monitor none \
  -qmp "unix:$SOCK,server,nowait" >"$qlog" 2>&1 &
QEMU_PID=$!
teardown() {
  kill -0 "$QEMU_PID" 2>/dev/null || return 0
  if [ "${TEST_KEEP:-0}" = 1 ] && [ "$1" = fail ]; then echo "TEST_KEEP=1: XP left running, QMP at $SOCK (pid $QEMU_PID)"; return 0; fi
  qmp json '{"execute":"system_powerdown"}' 2>/dev/null
  for _ in $(seq 60); do kill -0 "$QEMU_PID" 2>/dev/null || return 0; sleep 2; done
  echo "XP did not power down, killing"; kill "$QEMU_PID" 2>/dev/null
}
fail() { echo "FAIL: $*"; teardown fail; exit 1; }
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { tail -5 "$qlog"; fail "no QMP socket"; }
deadline=$(( $(date +%s) + ${BOOT_TIMEOUT:-300} )); t0=$(date +%s)
sleep 25
while ! mcopy -n -o -i "$fat" ::/OUT/STARTED.TXT "$OUT/STARTED.TXT" 2>/dev/null; do
  if [ "$(date +%s)" -ge "$deadline" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then tail -5 "$qlog"; fail "RUN.BAT did not start within the timeout"; fi
  qmp keys meta_l+r; sleep 1; qmp keys ctrl+a; qmp type 'E:\RUN.BAT'; qmp keys ret
  for _ in $(seq 10); do mcopy -n -o -i "$fat" ::/OUT/STARTED.TXT "$OUT/STARTED.TXT" 2>/dev/null && break; sleep 1; done
done
echo "RUN.BAT started after $(( $(date +%s) - t0 )) s"
deadline=$(( $(date +%s) + 600 ))
while ! mcopy -n -o -i "$fat" ::/OUT/DONE.TXT "$OUT/DONE.TXT" 2>/dev/null; do
  if [ "$(date +%s)" -ge "$deadline" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then tail -5 "$qlog"; fail "the copy did not finish within the timeout"; fi
  sleep 3
done
echo "copy done after $(( $(date +%s) - t0 )) s; shutting XP down"
teardown ok
rm -rf "$OUT/cd" "$OUT/DIR.TXT" "$OUT/XCOPY.TXT" "$OUT/XCOPYRC.TXT"; mkdir -p "$OUT/cd"
mcopy -n -i "$fat" ::/OUT/DIR.TXT ::/OUT/XCOPY.TXT ::/OUT/XCOPYRC.TXT "$OUT/" 2>/dev/null
mcopy -s -n -i "$fat" ::/CD/* "$OUT/cd/" 2>/dev/null
echo "xcopy: $(tr -d '\r' < "$OUT/XCOPY.TXT" | tail -1), rc $(tr -d '\r\n ' < "$OUT/XCOPYRC.TXT")"
echo "DIR D:\\ head:"; tr -d '\r' < "$OUT/DIR.TXT" | head -8 | sed 's/^/    /'
grep -i 'cdimage\|libdisc' "$qlog" | sed 's/^/    qemu: /'

# every reference file present with the same bytes (names case-insensitively: FAT/ISO 9660)
rc=0; n=0
while IFS= read -r -d '' f; do
  rel="${f#"$REF"/}"
  got="$(find "$OUT/cd" -ipath "$OUT/cd/$rel" -type f | head -1)"
  if [ -z "$got" ]; then echo "  missing: $rel"; rc=1; continue; fi
  if ! cmp -s "$f" "$got"; then echo "  differs: $rel ($(stat -c%s "$f") vs $(stat -c%s "$got") bytes)"; rc=1; fi
  n=$((n+1))
done < <(find "$REF" -type f -print0)
extra=$(( $(find "$OUT/cd" -type f | wc -l) - n ))
[ "$extra" -ne 0 ] && echo "  $extra file(s) on the copy that are not in the reference"
if [ $rc -eq 0 ]; then echo "PASS: $n files copied from the disc match the reference"; else echo "FAIL: differences above ($n reference files)"; fi
exit $rc
