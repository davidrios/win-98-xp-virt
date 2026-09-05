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
# CDTEST=<path to CDTEST.EXE>: also CD audio (doc 17 §5.4) — the drive gets
# `-device ide-cd,audiodev=` on a `-audiodev wav` so the tone the guest plays
# through MCI (CDTEST.EXE from the scratch disk after the copy) lands in
# <outdir>/cd.wav, whose loudest second must be a 1 kHz tone (the selftest
# disc's / `discx convert --audio tone.wav`'s track 2); cdtest.log is pulled.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
IMG="${1:?xp image}"; DISC="${2:?disc image}"; REF="${3:?reference dir}"; OUT="${4:-$ROOT/build/test/cdimage-xp}"
mkdir -p "$OUT"
for t in mkfs.fat sfdisk mcopy mmd; do command -v $t >/dev/null || { echo "needs $t"; exit 2; }; done
[ -x build/qemu/qemu-system-i386 ] || { echo "no build/qemu/qemu-system-i386"; exit 2; }
accel="${TEST_ACCEL:-}"; if [ -z "$accel" ]; then if [ -w /dev/kvm ]; then accel=kvm; else accel=tcg; fi; fi
cpu=(-cpu pentium3); [ "$accel" = kvm ] && cpu=(-cpu host)

# scratch FAT32 sized for the disc: the reference's bytes + 64 MB (sparse)
need=$(( ($(du -sb "$REF" | cut -f1) / 1048576 + 64) ))
scratch="$OUT/scratch.img"; rm -f "$scratch"; truncate -s "${need}M" "$scratch"
printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$scratch" >/dev/null
mkfs.fat -F 32 --offset 2048 "$scratch" >/dev/null
fat="$scratch@@1048576"
printf '@echo off\r\nmkdir E:\\OUT\r\necho started > E:\\OUT\\STARTED.TXT\r\ndir D:\\ /S > E:\\OUT\\DIR.TXT\r\nxcopy D:\\ E:\\CD\\ /I /Y /S /E /H > E:\\OUT\\XCOPY.TXT\r\necho %%ERRORLEVEL%% > E:\\OUT\\XCOPYRC.TXT\r\necho done > E:\\OUT\\DONE.TXT\r\n' > "$OUT/RUN.BAT"
mcopy -i "$fat" "$OUT/RUN.BAT" ::/RUN.BAT
cdargs=(-cdrom "$DISC")
if [ -n "${CDTEST:-}" ]; then
  [ -f "$CDTEST" ] || { echo "CDTEST=$CDTEST not found"; exit 2; }
  mcopy -i "$fat" "$CDTEST" ::/CDTEST.EXE
  printf '@echo off\r\nmkdir E:\\OUT\r\necho started > E:\\OUT\\STARTED.TXT\r\ndir D:\\ /S > E:\\OUT\\DIR.TXT\r\nxcopy D:\\ E:\\CD\\ /I /Y /S /E /H > E:\\OUT\\XCOPY.TXT\r\necho %%ERRORLEVEL%% > E:\\OUT\\XCOPYRC.TXT\r\ncd /d E:\\OUT\r\nE:\\CDTEST.EXE D 2 > E:\\OUT\\CDTEST.TXT\r\necho done > E:\\OUT\\DONE.TXT\r\n' > "$OUT/RUN.BAT"
  mcopy -o -i "$fat" "$OUT/RUN.BAT" ::/RUN.BAT
  rm -f "$OUT/cd.wav"
  cdargs=(-audiodev "wav,id=cd0,path=$OUT/cd.wav" -drive "if=none,id=cd0,media=cdrom,file=$DISC" -device "ide-cd,bus=ide.1,drive=cd0,audiodev=cd0")
fi

SOCK="$OUT/qmp.sock"; rm -f "$SOCK"; qlog="$OUT/qemu.log"
qmp() { python3 tools/qmpc.py "$SOCK" "$@" >/dev/null; }
echo "XP: $IMG (snapshot), disc: $DISC, accel: $accel"
build/qemu/qemu-system-i386 -L qemu/pc-bios -accel "$accel" "${cpu[@]}" -machine pc -m 512 \
  -drive "file=$IMG,if=ide,index=0,media=disk,snapshot=on" -drive "file=$scratch,format=raw,if=ide,index=1,media=disk" \
  "${cdargs[@]}" -vga cirrus -net none -usb -device usb-tablet -display none -serial none -monitor none \
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
[ "$n" -gt 0 ] || { echo "FAIL: no reference files under $REF (extract the ISO with bsdtar, or xorriso -osirrox on -indev x.iso -extract / dir)"; rc=1; }
if [ -n "${CDTEST:-}" ]; then
  mcopy -n -i "$fat" ::/OUT/cdtest.log "$OUT/cdtest.log" 2>/dev/null || mcopy -n -i "$fat" ::/OUT/CDTEST.LOG "$OUT/cdtest.log" 2>/dev/null
  echo "cdtest.log:"; tr -d '\r' < "$OUT/cdtest.log" 2>/dev/null | grep -E 'tracks|play|position|mode|RESULT|error' | head -30 | sed 's/^/    /'
  # the verdict: MCI saw the play running with advancing positions, and the tone is in the wav; the
  # script's last line is reported but not required (XP's mcicda has ended CDTEST.EXE silently after
  # a resume past the track's end)
  grep -q 'RESULT ok' "$OUT/cdtest.log" 2>/dev/null || echo "  note: CDTEST.EXE ended before its last line (last: $(tr -d '\r' < "$OUT/cdtest.log" 2>/dev/null | tail -1))"
  npos=$(tr -d '\r' < "$OUT/cdtest.log" 2>/dev/null | grep -c 'status cd position" -> "02:00:0[1-9]')
  if grep -q '"playing"' "$OUT/cdtest.log" 2>/dev/null && [ "${npos:-0}" -ge 1 ] && python3 - "$OUT/cd.wav" <<'PY'
import struct, sys, math
p = sys.argv[1]
d = open(p, 'rb').read()
i = d.find(b'data'); assert i > 0, "no data chunk"
n = struct.unpack('<I', d[i+4:i+8])[0]; pcm = d[i+8:i+8+n]
rate = struct.unpack('<I', d[24:28])[0]; ch = struct.unpack('<H', d[22:24])[0]
frames = len(pcm) // (2 * ch)
print("wav: %d frames, %d Hz, %d ch, %.1f s" % (frames, rate, ch, frames / rate))
best = (0, 0)
for sec in range(int(frames / rate)):
    s = pcm[sec * rate * 2 * ch:(sec + 1) * rate * 2 * ch]
    v = struct.unpack('<%dh' % (len(s) // 2), s)[::ch]
    rms = math.sqrt(sum(x * x for x in v) / len(v))
    if rms > best[0]: best = (rms, sec)
rms, sec = best
print("loudest second: %d (rms %.0f)" % (sec, rms))
assert rms > 500, "silence"
s = pcm[sec * rate * 2 * ch:(sec + 1) * rate * 2 * ch]
v = struct.unpack('<%dh' % (len(s) // 2), s)[::ch][:4096]
N = len(v); bestf = (0, 0)
for k in range(50, 3000, 10):
    re = sum(v[t] * math.cos(2 * math.pi * k * t / rate) for t in range(N))
    im = sum(v[t] * math.sin(2 * math.pi * k * t / rate) for t in range(N))
    m = re * re + im * im
    if m > bestf[0]: bestf = (m, k)
print("dominant frequency ~%d Hz" % bestf[1])
assert 950 <= bestf[1] <= 1050, "not the 1 kHz tone"
PY
  then echo "PASS: CD audio: the 1 kHz tone reached the audiodev"; else echo "FAIL: CD audio (cdtest.log / cd.wav under $OUT)"; rc=1; fi
fi
if [ $rc -eq 0 ]; then echo "PASS: $n files copied from the disc match the reference"; else echo "FAIL: differences above ($n reference files)"; fi
exit $rc
