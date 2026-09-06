#!/usr/bin/env bash
# Build the Win98/Me display driver for the d3dpt-vga adapter (doc 19,
# ADR-012 / M10): the 16-bit DIB Engine mini display driver d3dpt9x.drv and
# its INF, staged as guest-tools/out/driver9x/.
#
# Unlike everything else in guest-tools/, this needs **Open Watcom**, not
# mingw-w64: the target is a 16-bit NE module (and later a ring-0 VxD), and
# mingw can produce neither format. Point WATCOM at an installed tree —
#
#   WATCOM=$HOME/.local/opt/open-watcom guest-tools/build-driver9x.sh
#
# — or install one where this script looks by default. Open Watcom v2 ships
# Linux-hosted binaries: https://github.com/open-watcom/open-watcom-v2
# (the Last-CI-build release's ow-snapshot.tar.xz unpacks ready to use).
#
# The headers this builds against are in src/d3dptvid/ddk9x/ — no Microsoft
# DDK, same rule as the XP driver (doc 15). dibeng.lib is made here by wlib
# from a text import list, so the DIB Engine needs no DDK either.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/guest-tools/src/d3dptvid/w9x"
DDK="$ROOT/guest-tools/src/d3dptvid/ddk9x"
OUT="$ROOT/guest-tools/out/driver9x"
WATCOM="${WATCOM:-$HOME/.local/opt/open-watcom}"

[ -x "$WATCOM/binl64/wcc" ] || {
  echo "need Open Watcom (WATCOM=$WATCOM has no binl64/wcc)"
  echo "see the header of $0 for where to get it"
  exit 1
}
export WATCOM
export PATH="$WATCOM/binl64:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/win:$DDK"
export EDPATH="$WATCOM/eddat"
export WIPFC="$WATCOM/wipfc"

rm -rf "$OUT" && mkdir -p "$OUT"
BUILD="$OUT/obj"
mkdir -p "$BUILD"

# 16-bit, Windows entry conventions (-zW), loadds on exports (-zu is for
# the DS != SS the DLL model needs), stack checking off, small model.
# -wcd=303: the Windows entry points have signatures we do not choose, so
# an unused parameter is not news. wcc drops its .err beside the cwd, hence
# the subshells.
CFLAGS=(-q -wx -wcd=303 -s -zu -zls -zW -6 -fp6 -I"$DDK" -I"$SRC")

echo "==> d3dpt9x.obj"
( cd "$BUILD" && wcc "${CFLAGS[@]}" -fo=d3dpt9x.obj "$SRC/d3dpt9x.c" )
echo "==> dibthunk.obj"
( cd "$BUILD" && wasm -q -fo=dibthunk.obj "$SRC/dibthunk.asm" )

# The resource blobs: GDI and USER read the machine metrics, the colour
# table and the system fonts out of the .drv itself (doc 19). Each is a C
# file of pure data, linked as a raw DOS binary to get the bytes out.
for r in config colortab fonts fonts120; do
  echo "==> res/$r.bin"
  ( cd "$BUILD" && wcc -q -zu -zls -6 -I"$DDK" -I"$SRC/res" -fo=$r.obj "$SRC/res/$r.c" \
    && wlink op quiet disable 1014, 1023 name $r.bin sys dos output raw file $r.obj )
done

echo "==> d3dpt9x.res"
( cd "$BUILD" && cp "$SRC/res/d3dpt9x.rc" . && wrc -q -r -ad -bt=windows -fo=d3dpt9x.res \
    -I"$WATCOM/h/win" d3dpt9x.rc )

echo "==> dibeng.lib (import library, from the text list)"
( cd "$BUILD" && wlib -b -q -n -fo -ii @"$DDK/dibeng.lbc" dibeng.lib >/dev/null )

echo "==> d3dpt9x.drv (16-bit NE, module DISPLAY)"
# wlink reads its directives from a real file (@name), not a pipe.
cat > "$BUILD/d3dpt9x.lnk" <<'LNK'
system windows dll initglobal
file d3dpt9x.obj
file dibthunk.obj
name d3dpt9x.drv
option map=d3dpt9x.map
library dibeng.lib
option oneautodata
option nodefaultlibs
option modname=DISPLAY
option description 'DISPLAY : 100, 96, 96 : 2ksbox d3dpt-vga mini display driver.'
segment type data preload fixed
segment '_TEXT' preload shared
export BitBlt.1
export ColorInfo.2
export Control.3
export Disable.4
export Enable.5
export EnumDFonts.6
export EnumObj.7
export Output.8
export Pixel.9
export RealizeObject.10
export StrBlt.11
export ScanLR.12
export DeviceMode.13
export ExtTextOut.14
export GetCharWidth.15
export DeviceBitmap.16
export FastBorder.17
export SetAttribute.18
export DibBlt.19
export CreateDIBitmap.20
export DibToDevice.21
export SetPalette.22
export GetPalette.23
export SetPaletteTranslate.24
export GetPaletteTranslate.25
export UpdateColors.26
export StretchBlt.27
export StretchDIBits.28
export SelectBitmap.29
export BitmapBits.30
export ReEnable.31
export Inquire.101
export SetCursor.102
export MoveCursor.103
export CheckCursor.104
export GetDriverResourceID.450
export UserRepaintDisable.500
export ValidateMode.700
import GlobalSmartPageLock KERNEL.230
LNK
( cd "$BUILD" && wlink op quiet, start=DriverInit_ disable 2055 @d3dpt9x.lnk )

echo "==> resources into the module"
( cd "$BUILD" && wrc -q d3dpt9x.res d3dpt9x.drv )

# The linker stamps "expected Windows 3.00" into the NE header. GDI loads the
# driver either way, but the DIB Engine treats a 3.x module as a 3.x driver
# (vmdisp9x hits the same thing and calls the fix -40): say 4.00.
python3 - "$BUILD/d3dpt9x.drv" <<'PYFIX'
import struct, sys
p = sys.argv[1]
f = bytearray(open(p, 'rb').read())
ne = struct.unpack_from('<I', f, 0x3c)[0]
assert f[ne:ne+2] == b'NE', 'not an NE module'
f[ne+0x3e] = 0        # minor
f[ne+0x3f] = 4        # major -> 4.00
open(p, 'wb').write(f)
PYFIX

cp "$BUILD/d3dpt9x.drv" "$OUT/"
cp "$SRC/d3dpt9x.inf" "$OUT/" 2>/dev/null || true

echo
echo "==> $OUT/d3dpt9x.drv"
ls -la "$OUT"
