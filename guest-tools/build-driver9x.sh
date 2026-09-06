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
option heapsize=0
option nodefaultlibs
option modname=DISPLAY
option description 'DISPLAY : 100, 96, 96 : 2ksbox d3dpt-vga mini display driver.'
segment class 'DATA' preload fixed
segment type data preload fixed
segment '_TEXT' preload fixed shared
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

# The ring-0 half: a dynamically loadable VxD (LE), 32-bit flat, no CRT.
# -zls drops the runtime references, -s the stack checks; the segments are
# PRELOAD NONDISCARDABLE because a mini-VDD must stay resident.
echo "==> d3dptvxd.obj (32-bit, ring 0)"
( cd "$BUILD" && wcc386 -q -wx -wcd=303 -s -zls -mf -6s -fp6 -ei -zp1 \
    -I"$DDK" -I"$SRC" -fo=d3dptvxd.obj "$SRC/d3dptvxd.c" )

echo "==> d3dpt9v.vxd"
cat > "$BUILD/d3dpt9v.lnk" <<'LNK'
system win_vxd dynamic
option map=d3dpt9v.map
option nodefaultlibs
name d3dpt9v.vxd
file d3dptvxd.obj
segment '_LTEXT' PRELOAD NONDISCARDABLE IOPL
segment '_TEXT'  PRELOAD NONDISCARDABLE IOPL
segment '_DATA'  PRELOAD NONDISCARDABLE IOPL
segment 'CONST'  PRELOAD NONDISCARDABLE IOPL
segment 'CONST2' PRELOAD NONDISCARDABLE IOPL
export VXD_DDB.1
LNK
( cd "$BUILD" && wlink op quiet @d3dpt9v.lnk )

# wlink's LE output is not quite a VxD yet. The fix is the object table,
# not the header (this is what vmdisp9x's `fixlink -vxd32` does): every
# object must be marked executable, and every object's base virtual
# address must be zero, because a VxD is flat — all its pages start at the
# beginning. The module-type field is set to what a real Windows 98 VxD
# carries as well (checked against the guest's own VJOYD.VXD and
# FXMEMMAP.VXD, both 0x38000).
python3 - "$BUILD/d3dpt9v.vxd" <<'PYVXD'
import struct, sys
p = sys.argv[1]
f = bytearray(open(p, 'rb').read())
le = struct.unpack_from('<I', f, 0x3c)[0]
assert f[le:le+2] == b'LE', 'not an LE module: %r' % f[le:le+2]

flags = struct.unpack_from('<I', f, le + 0x10)[0]
struct.pack_into('<I', f, le + 0x10, flags | 0x00038000)

# The entry table's one bundle exports the DDB. wlink types it as a
# 16-bit/call-gate entry because the symbol is data; a real VxD's is a
# 32-bit entry (FXMEMMAP.VXD again). The record bytes are the same either
# way when the offset is zero, so this is just the type byte.
ent = le + struct.unpack_from('<I', f, le + 0x5c)[0]
if f[ent] == 1 and f[ent+1] == 2:
    f[ent+1] = 3
    print("   entry table: 16-bit bundle -> 32-bit")

objtab = le + struct.unpack_from('<I', f, le + 0x40)[0]
nobj   = struct.unpack_from('<I', f, le + 0x44)[0]
for i in range(nobj):
    o = objtab + i * 24                       # sizeof(LE_object)
    size, addr, oflags = struct.unpack_from('<III', f, o)
    struct.pack_into('<II', f, o + 4, 0, oflags | 0x0004)   # addr = 0, executable
    print("   object %d: size %d, addr %d -> 0, flags %08x -> %08x"
          % (i, size, addr, oflags, oflags | 4))
open(p, 'wb').write(f)
PYVXD

# The DDB must sit at offset 0 of the VxD's one object: that is where the
# entry table points and where the VMM's loader looks. Anything the compiler
# emits into the same segment ahead of it — a string literal, a const array —
# pushes it off, and the VMM then declines the module in complete silence
# (doc 19 Section 12). The map says where it went, so the build checks.
awk '/^0001:00000000 +VXD_DDB$/ { found = 1 }
     END { if (!found) {
        print "d3dpt9v.vxd: VXD_DDB is not at 0001:00000000 — the VMM will"
        print "  ignore this module without a word. Something in _LTEXT is"
        print "  emitted ahead of it; see the map:"
        exit 1 } }' "$BUILD/d3dpt9v.map" || {
  grep -m5 -A4 'Module: d3dptvxd' "$BUILD/d3dpt9v.map"
  exit 1
}

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
# A display driver's DGROUP is PRELOAD FIXED SINGLE with no local heap
# (CIRRUSMM.DRV and every other one on the guest say 0); wlink insists on
# giving a DLL 1 KiB and `option heapsize=0` does not move it.
struct.pack_into('<H', f, ne + 0x10, 0)
open(p, 'wb').write(f)
PYFIX

# Every internal relocation must name a segment the NE segment table has.
# wlink can emit one that does not: a segment that ends up empty is dropped
# from the table while the relocations that referenced it keep its old
# index, and KERNEL then refuses the whole module without a word — which is
# exactly how doc 19 Section 13 was spent. Nothing downstream complains, so
# the build does.
python3 - "$BUILD/d3dpt9x.drv" <<'PYCHK'
import struct, sys
p = sys.argv[1]
f = open(p, 'rb').read()
ne = struct.unpack_from('<I', f, 0x3c)[0]
segcount,  = struct.unpack_from('<H', f, ne + 0x1c)
segtaboff, = struct.unpack_from('<H', f, ne + 0x22)
align = 1 << (struct.unpack_from('<H', f, ne + 0x32)[0] or 9)
bad = 0
for s in range(segcount):
    off, size, flags, _ = struct.unpack_from('<HHHH', f, ne + segtaboff + s * 8)
    if not flags & 0x0100:                     # no relocation records
        continue
    base = off * align + size
    n, = struct.unpack_from('<H', f, base)
    for i in range(n):
        r = base + 2 + i * 8
        if f[r + 1] & 3:                       # not an internal reference
            continue
        seg = f[r + 4]
        if seg != 0xff and not 1 <= seg <= segcount:
            print("   segment %d relocation at 0x%04x names segment %d of %d"
                  % (s + 1, struct.unpack_from('<H', f, r + 2)[0], seg, segcount))
            bad += 1
if bad:
    sys.exit("d3dpt9x.drv: %d dangling relocation(s) — KERNEL will refuse to "
             "load this module. Check the .map for an empty segment." % bad)
PYCHK

cp "$BUILD/d3dpt9x.drv" "$BUILD/d3dpt9v.vxd" "$OUT/"
cp "$SRC/d3dpt9x.inf" "$OUT/" 2>/dev/null || true

echo
echo "==> $OUT/d3dpt9x.drv"
ls -la "$OUT"
