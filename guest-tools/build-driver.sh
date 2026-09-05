#!/usr/bin/env bash
# Build the XP display driver for the d3dpt-vga adapter (doc 15, ADR-008 /
# M7a): the video miniport d3dptvid.sys, the display driver d3dptdisp.dll,
# the INF and the DRVINST.EXE installer, staged as guest-tools/out/driver/
# and a small ISO (guest-tools/out/d3dpt-driver.iso) for fast iteration.
# build-wrappers.sh calls this too and copies the folder into the big ISO
# as DRIVER\.
#
# Kernel-mode PE files with GCC: no CRT (-nostdlib -ffreestanding), the
# native subsystem, an explicit entry point, imports only from
# videoprt.sys (miniport) / win32k.sys (display driver) — mingw-w64 ships
# the DDK headers and import libraries. Same toolchain as the wrappers;
# needs i686-w64-mingw32-gcc, xorriso (or genisoimage/mkisofs) for the ISO.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/guest-tools/src/d3dptvid"
OUT="$ROOT/guest-tools/out/driver"
CC=i686-w64-mingw32-gcc
command -v "$CC" >/dev/null || { echo "need $CC (mingw-w64)"; exit 1; }
# The headers sit under the toolchain's sysroot: /usr/i686-w64-mingw32 on
# Arch, Cellar/mingw-w64/<ver>/toolchain-i686/i686-w64-mingw32 on Homebrew
# (where the compiler in /opt/homebrew/bin is only a symlink, so deriving the
# prefix from the binary's location finds nothing).
DDK_INC="$("$CC" -print-sysroot 2>/dev/null)/i686-w64-mingw32/include/ddk"
[ -f "$DDK_INC/video.h" ] || DDK_INC="$(dirname "$(dirname "$(command -v "$CC")")")/i686-w64-mingw32/include/ddk"
[ -f "$DDK_INC/video.h" ] || DDK_INC="/usr/i686-w64-mingw32/include/ddk"
[ -f "$DDK_INC/video.h" ] || { echo "mingw-w64 DDK headers (ddk/video.h) not found"; exit 1; }

# kernel mode: no stack probes (no __chkstk in the kernel), no stack
# protector, no unwind tables; -lgcc for the odd helper (64-bit shifts).
KFLAGS=(-O2 -Wall -Wno-unused-function -nostdlib -shared -ffreestanding
        -fno-stack-protector -mno-stack-arg-probe -fno-asynchronous-unwind-tables
        -fno-ident -march=pentium3 -mtune=generic -fno-tree-loop-distribute-patterns
        -Wl,--enable-stdcall-fixup
        -Wl,--subsystem,native -Wl,--image-base,0x10000
        -Wl,--major-os-version,5 -Wl,--minor-os-version,1
        -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1)

rm -rf "$OUT" && mkdir -p "$OUT"
echo "==> d3dptvid.sys (video miniport)"
"$CC" "${KFLAGS[@]}" -I"$DDK_INC" -Wl,--entry,_DriverEntry@8 -Wl,--exclude-all-symbols \
  -o "$OUT/d3dptvid.sys" "$SRC/d3dptvid.c" "$SRC/kcrt.c" -lvideoprt -lntoskrnl -lgcc
echo "==> d3dptdisp.dll (display driver)"
"$CC" "${KFLAGS[@]}" -I"$SRC/ddk" -Wl,--entry,_DrvEnableDriver@12 -Wl,--kill-at \
  -o "$OUT/d3dptdisp.dll" "$SRC/d3dptdisp.c" "$SRC/kcrt.c" "$SRC/d3dptdisp.def" -lwin32k -lgcc
echo "==> drvinst.exe (installer, user mode, msvcrt)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/drvinst.exe" "$SRC/drvinst.c" -ladvapi32 -luser32
echo "==> setmode.exe (mode switch from a script)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/setmode.exe" "$SRC/setmode.c" -luser32
echo "==> d3d7test.exe (Direct3D 7 HAL test)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/d3d7test.exe" "$SRC/d3d7test.c" -lddraw -ldxguid -lgdi32 -luser32
echo "==> ddtest.exe (DirectDraw 7 flip-chain test)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/ddtest.exe" "$SRC/ddtest.c" -lddraw -ldxguid -lgdi32 -luser32
echo "==> ditest.exe (DirectInput keyboard under load)"
"$CC" -O2 -Wall -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os -march=pentium3 -mtune=generic \
  -o "$OUT/ditest.exe" "$SRC/ditest.c" -ldinput -lddraw -ldxguid -lgdi32 -luser32
cp "$SRC/d3dptvid.inf" "$OUT/d3dptvid.inf"

# sanity: the kernel modules import only from their port driver, and nothing
# links the CRT (no import at all is the expected answer for the .sys/.dll)
check_imports() {  # file, allowed DLLs (regex, case-insensitive)
  local bad
  bad="$(i686-w64-mingw32-objdump -p "$1" | awk '/DLL Name:/ {print $3}' | grep -ivE "^($2)\$" || true)"
  [ -z "$bad" ] || { echo "ERROR: $1 imports from $bad (expected only $2)"; exit 1; }
}
check_imports "$OUT/d3dptvid.sys" "videoprt.sys|ntoskrnl.exe"
check_imports "$OUT/d3dptdisp.dll" win32k.sys
for f in "$OUT"/*.sys "$OUT"/*.dll "$OUT"/*.exe; do
  if i686-w64-mingw32-objdump -p "$f" | grep -q 'api-ms-win-crt'; then
    echo "ERROR: $f links against the UCRT"; exit 1
  fi
  n=$(i686-w64-mingw32-objdump -d "$f" | grep -cE '\b(movdq[au]|movapd|movupd|pshufd|punpck[hl](bw|wd|dq|qdq)|paddq|cvtsd2|cvtsi2sd|cvttsd2si|movsd[[:space:]]+%xmm|xorpd|andpd|popcnt|ptest|pcmpistr|pshufb|pmaddubsw)\b' || true)
  [ "$n" -eq 0 ] || { echo "ERROR: $f contains $n SSE2+/POPCNT instructions (pentium3 floor)"; exit 1; }
done

crlf() { awk '{ sub(/\r$/, ""); printf "%s\r\n", $0 }'; }
crlf > "$OUT/README.TXT" <<'TXT'
win98-xp-virt paravirtual display adapter driver (d3dpt-vga), Windows 2000/XP.

Boot XP with:  -vga none -device d3dpt-vga
Install (as Administrator):  DRVINST.EXE -reboot
  (or Device Manager > Video Controller (VGA Compatible) > Update Driver
   > Install from a specific location > this folder)
Then Display Properties offers the host's mode table (640x480 .. 1600x1200,
8/16/32 bpp, 60/75/85 Hz; 8 bpp is palettized); the desktop lives in the adapter's VRAM and the
player shows it without copies.

Files: D3DPTVID.SYS (video miniport), D3DPTDISP.DLL (display driver),
D3DPTVID.INF, DRVINST.EXE (scripted installer: sets the driver-signing
policy to ignore, UpdateDriverForPlugAndPlayDevices, optional reboot),
SETMODE.EXE (lists the modes; SETMODE 1024 768 32 85 switches and saves),
DDTEST.EXE (DirectDraw 7: HAL caps, exclusive flip chain, Lock/Blt/Flip,
fps, at 8 bpp a palette on the primary rotated every frame; DDTEST [w h bpp]
[frames]; log in ddtest.log),
DITEST.EXE (DirectInput keyboard under load: DITEST [seconds] [busy-ms]
[-window] [-nonexcl]; what DirectInput / GetAsyncKeyState / WM_KEYDOWN
each see of the keys, log in ditest.log).
TXT
crlf < "$SRC/d3dptvid.inf" > "$OUT/d3dptvid.inf"

# 8.3 upper-case names for the ISO folder
( cd "$OUT" && for f in *; do u="$(echo "$f" | tr a-z A-Z)"; [ "$f" = "$u" ] || mv "$f" "$u"; done )
ls -la "$OUT"

ISO="$ROOT/guest-tools/out/d3dpt-driver.iso"
STAGE="$ROOT/guest-tools/out/driver-iso"; rm -rf "$STAGE"; mkdir -p "$STAGE/DRIVER"; cp "$OUT"/* "$STAGE/DRIVER/"
if command -v xorriso >/dev/null; then
  xorriso -as mkisofs -o "$ISO" -V "D3DPTDRV" -J -r "$STAGE" >/dev/null 2>&1
elif command -v genisoimage >/dev/null; then
  genisoimage -o "$ISO" -V "D3DPTDRV" -J -r "$STAGE" >/dev/null 2>&1
elif command -v mkisofs >/dev/null; then
  mkisofs -o "$ISO" -V "D3DPTDRV" -J -r "$STAGE" >/dev/null 2>&1
else
  echo "no ISO tool found; staged files are in $OUT"; exit 0
fi
echo "==> $ISO"
