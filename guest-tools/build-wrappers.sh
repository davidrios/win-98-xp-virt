#!/usr/bin/env bash
# Build qemu-3dfx guest wrappers (Windows DLLs) from the SAME
# third_party/qemu-3dfx commit our QEMU fork is signed with, plus the
# WineD3D wrapper set (JHRobotics' wine9x, a Wine 1.7.55 port for
# 95/98/Me/XP, LGPL) and a Direct3D 9 smoke test, and stage them as a
# guest-tools ISO. Needs: i686-w64-mingw32-gcc, gendef, xxd, shasum,
# git, make, nasm; xorriso or genisoimage/mkisofs for the ISO.
#   Linux (Arch):  pacman -S mingw-w64-gcc mingw-w64-tools xorriso
#   macOS:         brew install mingw-w64 xorriso
# DOS-only pieces (GLIDE2X.OVL via Open Watcom, DJGPP DXEs) are skipped.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FX="$ROOT/third_party/qemu-3dfx"
OUT="$ROOT/guest-tools/out"
REV="$(git -C "$FX" rev-parse --short HEAD)"

# gendef (mingw-w64-tools) is not in Homebrew's mingw-w64; build it from
# pinned upstream sources when missing. It is a standalone C program.
GENDEF_SRC_REF="93f3505a758fe70e56678f00e753af3bc4f640bb"   # mirror/mingw-w64 master, 2024-09-24
ensure_gendef() {
  if command -v gendef >/dev/null && [ -z "${GENDEF_FORCE_BUILD:-}" ]; then return; fi
  local d="$ROOT/guest-tools/tools" bin="$ROOT/guest-tools/tools/bin"
  if [ ! -x "$bin/gendef" ]; then
    echo "==> building gendef from mingw-w64 @ ${GENDEF_SRC_REF:0:7}"
    mkdir -p "$d/gendef-src" "$bin"
    local base="https://raw.githubusercontent.com/mirror/mingw-w64/$GENDEF_SRC_REF/mingw-w64-tools/gendef/src"
    for f in compat_string.c compat_string.h fsredir.c fsredir.h gendef.c gendef.h gendef_def.c; do
      curl -fsSL -o "$d/gendef-src/$f" "$base/$f"
    done
    cc -O2 -w -DVERSION='"1.0-win98xp"' -o "$bin/gendef" "$d"/gendef-src/*.c
  fi
  export PATH="$bin:$PATH"
}
ensure_gendef

# The wrapper Makefiles call bare `objdump` (exports-check counts PE export
# entries) and lean on GNU sed. On macOS, `objdump` is Apple's LLVM one and
# can't read PE the same way: route it to the mingw cross binutils' GNU
# objdump, and prefer gnu-sed when installed.
ensure_gnu_tools() {
  local bin="$ROOT/guest-tools/tools/bin"; mkdir -p "$bin"
  if ! objdump --version 2>/dev/null | grep -q 'GNU objdump'; then
    local xd; xd="$(command -v i686-w64-mingw32-objdump || true)"
    [ -n "$xd" ] || { echo "need GNU objdump (i686-w64-mingw32-objdump from mingw-w64)"; exit 1; }
    ln -sf "$xd" "$bin/objdump"
  fi
  if [ "$(uname -s)" = Darwin ]; then
    local g; g="$(brew --prefix gnu-sed 2>/dev/null || true)/libexec/gnubin"
    [ -d "$g" ] && export PATH="$g:$PATH"
  fi
  export PATH="$bin:$PATH"
}
ensure_gnu_tools

# Win9x has no UCRT. Modern mingw-w64 (Homebrew, Arch) defaults to UCRT, so
# force classic msvcrt: msvcrt-mode headers + link msvcrt.dll (import lib
# libmsvcrt-os.a on UCRT-default toolchains). Done via a compiler shim so
# qemu-3dfx's Makefiles need no changes; the same flags build wglgears.
MSVCRT_FLAGS="-D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os"
# qemu-3dfx's Makefiles use -march=x86-64-v2 (SSE4.2/POPCNT); our reference
# guests are pentium2/pentium3 class (doc 06) and would #UD. Appended AFTER
# the Makefile's flags so it wins: Pentium III floor (SSE1; doubles on x87).
ARCH_FLAGS="-march=pentium3 -mtune=generic"
ensure_msvcrt_cc() {
  local bin="$ROOT/guest-tools/tools/bin" real
  # A shim from a previous run may already be on PATH (tools/bin gets
  # prepended by the gendef/objdump steps): remove it first, or `command -v`
  # would find the shim itself and it would exec itself forever
  # ("argument list too long").
  rm -f "$bin/i686-w64-mingw32-gcc"
  real="$(command -v i686-w64-mingw32-gcc || true)"
  case "$real" in "$bin"/*) echo "internal error: shim resolved to itself"; exit 1;; esac
  [ -n "$real" ] || { echo "need i686-w64-mingw32-gcc (mingw-w64)"; exit 1; }
  REAL_CC="$real"
  [ -f "$(dirname "$(dirname "$real")")/i686-w64-mingw32/lib/libmsvcrt-os.a" ] || \
  [ -f "$(dirname "$real")/../i686-w64-mingw32/lib/libmsvcrt-os.a" ] || \
    echo "warning: libmsvcrt-os.a not found next to the toolchain; msvcrt link may fail"
  printf '#!/usr/bin/env bash\nexec "%s" %s "$@" %s\n' "$real" "$MSVCRT_FLAGS" "$ARCH_FLAGS" > "$bin/i686-w64-mingw32-gcc"
  chmod +x "$bin/i686-w64-mingw32-gcc"
}
ensure_msvcrt_cc

check_isa() {  # fail loudly on SSE2+ / POPCNT (guest CPU floor is pentium3)
  local n; n=$(objdump -d "$1" | grep -cE '\b(movdq[au]|movapd|movupd|pshufd|punpck[hl](bw|wd|dq|qdq)|paddq|cvtsd2|cvtsi2sd|cvttsd2si|movsd[[:space:]]+%xmm|xorpd|andpd|popcnt|ptest|pcmpistr|pshufb|pmaddubsw)\b' || true)
  if [ "$n" -gt 0 ]; then echo "ERROR: $1 contains $n SSE2+/POPCNT instructions (pentium3 floor)"; exit 1; fi
}
check_crt() {  # fail loudly if anything still imports the UCRT api-sets
  if objdump -p "$1" | grep -q 'api-ms-win-crt'; then
    echo "ERROR: $1 links against the UCRT (not loadable on Win9x)"; exit 1
  fi
}

# WineD3D for the guests: wine9x builds wined3d.dll (Wine 1.7.55 with the
# 9x/XP fixes) plus the DX interfaces wined8/wined9/winedd and per-OS
# "switcher" ddraw/d3d8/d3d9 DLLs for a system-wide install. wined3d links
# the CRT, so it gets the msvcrt flags; not the shim's -march=pentium3
# though, with which GCC emits a memset call inside the CRT-less switcher
# DLLs (wine9x's own -march=pentium2 is below our floor anyway, and the
# ISA check below covers the result). Its pthread9x sub-build hardcodes
# the host `ar`, which is BSD ar on macOS.
WINE9X_URL="https://github.com/JHRobotics/wine9x.git"
WINE9X_REF="8ab16c6c0930efc1f9138eddda7b3114d7f31e62"   # main, 2026-09 (v1.7.55.45 + tray/HAL change)
build_wined3d() {
  local d="$OUT/wine9x"
  if [ ! -d "$d/.git" ]; then
    echo "==> cloning wine9x @ ${WINE9X_REF:0:7}"
    git clone -q "$WINE9X_URL" "$d"
  fi
  ( cd "$d" && git fetch -q origin && git checkout -q -- . && git checkout -q "$WINE9X_REF" \
    && git submodule update --init -q )
  # our wine9x patch queue (patches/wine9x/*.patch, git-format, from pristine)
  for p in "$ROOT"/patches/wine9x/*.patch; do
    [ -e "$p" ] || continue
    ( cd "$d" && git apply --check "$p" && git apply "$p" ) && echo "    wine9x: $(basename "$p") applied" \
      || { echo "wine9x patch $(basename "$p") does not apply"; exit 1; }
  done
  cp "$d/config.mk-sample" "$d/config.mk"
  echo "==> building wine9x (wined3d + DX interfaces + switchers)"
  ( cd "$d" && make clean >/dev/null 2>&1; make -C pthread9x clean >/dev/null 2>&1
    make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
      all d3d8_xp.dll d3d9_xp.dll ddraw_xp.dll d3d8_98.dll d3d9_98.dll ddraw_98.dll \
      CC="$REAL_CC $MSVCRT_FLAGS" LD="$REAL_CC -mcrtdll=msvcrt-os" \
      LIBSTATIC='i686-w64-mingw32-ar rcs -o $@' > "$d/build.log" 2>&1 ) \
    || { echo "wine9x build failed, see $d/build.log"; tail -20 "$d/build.log"; exit 1; }
}

build_wrapper() {  # $1 = 3dfx | mesa
  local d="$FX/wrappers/$1/build"
  rm -rf "$d" && mkdir -p "$d"
  ( cd "$d" && bash "$FX/scripts/conf_wrapper" >/dev/null && make )   # serial: 'fxlib' step must precede objects
}

echo "==> qemu-3dfx commit $REV"
build_wrapper 3dfx
build_wrapper mesa
build_wined3d

rm -rf "$OUT/iso" && mkdir -p "$OUT/iso/WIN9X" "$OUT/iso/WIN2KXP" "$OUT/iso/GAMEDIR" "$OUT/iso/WINED3D"
G="$FX/wrappers/3dfx/build"; M="$FX/wrappers/mesa/build"
cp "$G"/glide.dll "$G"/glide2x.dll "$G"/glide3x.dll "$G"/fxmemmap.vxd "$OUT/iso/WIN9X/"
cp "$G"/glide.dll "$G"/glide2x.dll "$G"/glide3x.dll "$G"/fxptl.sys "$G"/instdrv.exe "$OUT/iso/WIN2KXP/"
cp "$M"/opengl32.dll "$OUT/iso/GAMEDIR/"
# WineD3D: per-game copies under the names games load (the Wine DX
# interfaces export the real entry points and import wined3d.dll by name),
# and the full set + switchers for a system-wide install per wine9x README.
W="$OUT/wine9x"
cp "$W"/wined9.dll "$OUT/iso/GAMEDIR/d3d9.dll"
cp "$W"/wined8.dll "$OUT/iso/GAMEDIR/d3d8.dll"
cp "$W"/winedd.dll "$OUT/iso/GAMEDIR/ddraw.dll"
cp "$W"/wined3d.dll "$OUT/iso/GAMEDIR/"
cp "$W"/wined3d.dll "$W"/winedd.dll "$W"/wined8.dll "$W"/wined9.dll \
   "$W"/ddraw_xp.dll "$W"/d3d8_xp.dll "$W"/d3d9_xp.dll \
   "$W"/ddraw_98.dll "$W"/d3d8_98.dll "$W"/d3d9_98.dll "$OUT/iso/WINED3D/"
cp "$W"/README.md "$OUT/iso/WINED3D/WINE9X.TXT"
# D3D9 smoke test (guest-tools/src/d3d9test.c): adapter string, HAL caps,
# x87 control word after CreateDevice, spinning triangle with fps.
i686-w64-mingw32-gcc -O2 -o "$OUT/iso/GAMEDIR/d3d9test.exe" "$ROOT/guest-tools/src/d3d9test.c" \
  -ld3d9 -lgdi32 -luser32
# Display-mode probe (guest-tools/src/modetest.c): current mode, mode list,
# ChangeDisplaySettingsEx results for the switches ddraw/wined3d make.
i686-w64-mingw32-gcc -O2 -o "$OUT/iso/GAMEDIR/modetest.exe" "$ROOT/guest-tools/src/modetest.c" -luser32
# Reference workloads (doc 14 P0a): the same deterministic game-like scene on
# Direct3D 9 and Direct3D 8; -frames N -dump N x.bmp for golden images.
i686-w64-mingw32-gcc -O2 -o "$OUT/iso/GAMEDIR/d3dgame9.exe" "$ROOT/guest-tools/src/d3dgame9.c" -ld3d9 -lgdi32 -luser32
i686-w64-mingw32-gcc -O2 -o "$OUT/iso/GAMEDIR/d3dgame8.exe" "$ROOT/guest-tools/src/d3dgame8.c" -ld3d8 -lgdi32 -luser32
# Paravirtual Direct3D 9 (doc 14, d3dpt/): our d3d9.dll over the d3dpt
# device, with qemu-3dfx's fxlib device mapper (FXPTL.SYS / FXMEMMAP.VXD).
# Staged apart from WineD3D so the two stacks never mix in one folder;
# D3D9TEST.EXE and D3DGAME9.EXE next to it run against the device.
# d3d9_vtbl.h is generated from mingw's d3d9.h (gen_vtbl.py) and checked in.
mkdir -p "$OUT/iso/D3DPT"
i686-w64-mingw32-gcc -O2 -Wall -shared -o "$OUT/iso/D3DPT/d3d9.dll" "$ROOT/guest-tools/src/d3dpt/d3d9.c" \
  "$FX/wrappers/fxlib/fxlibnt.c" "$FX/wrappers/fxlib/fxlib9x.c" -I"$FX/wrappers/fxlib" \
  -Wl,--kill-at -lgdi32 -luser32
cp "$OUT/iso/GAMEDIR/d3d9test.exe" "$OUT/iso/GAMEDIR/d3dgame9.exe" "$OUT/iso/D3DPT/"
# Feature test (doc 14 P3): shaders, declarations, state blocks, queries, cube maps, surfaces.
i686-w64-mingw32-gcc -O2 -o "$OUT/iso/D3DPT/d3dfeat9.exe" "$ROOT/guest-tools/src/d3dfeat9.c" -ld3d9 -lgdi32 -luser32
# GL smoke test: Mesa's wglgears, ships in qemu-3dfx's demos. Run it next to
# OPENGL32.DLL inside the guest; the title/console shows the renderer.
i686-w64-mingw32-gcc -O2 -o "$OUT/iso/GAMEDIR/wglgears.exe" "$FX/wrappers/mesa/demos/wglgears.c" \
  -lopengl32 -lgdi32 -lglu32 -mwindows
for f in "$OUT"/iso/*/*.dll "$OUT"/iso/*/*.exe; do check_crt "$f"; check_isa "$f"; done
# CRLF: Win9x Notepad shows LF-only text as one line
crlf() { awk '{ sub(/\r$/, ""); printf "%s\r\n", $0 }'; }
crlf > "$OUT/iso/README.TXT" <<TXT
qemu-3dfx guest wrappers, built from qemu-3dfx commit $REV
(must match the host QEMU build's sign_commit stamp).

WIN9X\   -> copy GLIDE.DLL GLIDE2X.DLL GLIDE3X.DLL FXMEMMAP.VXD to C:\WINDOWS\SYSTEM
WIN2KXP\ -> copy GLIDE*.DLL to %SystemRoot%\system32, FXPTL.SYS to
            %SystemRoot%\system32\drivers, then run INSTDRV.EXE as Administrator.
            REQUIRED FOR OPENGL32.DLL TOO on 2000/XP: the wrapper maps the
            device through FXPTL.SYS (\\.\MAPMEM); without it OPENGL32.DLL
            refuses to load and every GL/D3D program fails at startup
            (0xc0000142 / "failed to initialize"). Win9x uses FXMEMMAP.VXD.
GAMEDIR\ -> copy OPENGL32.DLL next to each OpenGL game's EXE (Quake 2, etc.)
            WGLGEARS.EXE + OPENGL32.DLL in one folder = quick GL pass-through test
            Direct3D 8/9 games: also copy D3D8.DLL or D3D9.DLL + WINED3D.DLL
            (WineD3D, renders through OPENGL32.DLL) next to the game's EXE.
            DirectX 5/6/7 games (DirectDraw + Direct3D up to 7): DDRAW.DLL +
            WINED3D.DLL instead. Never copy DDRAW.DLL into system32 this way.
            D3D9TEST.EXE + D3D9.DLL + WINED3D.DLL + OPENGL32.DLL = D3D9 test
            MODETEST.EXE prints the display modes the driver accepts (run
            it when a fullscreen game fails to start).
            D3DGAME9.EXE / D3DGAME8.EXE: the reference scene (doc 14). Run
            on real hardware first: D3DGAME9 -frames 600 -dump 300 g9.bmp
            (and -fs, -bpp16, -shader variants) gives the golden images the
            emulated paths are compared against. WASD/arrows/Q/E camera,
            F1 wireframe, Space pause, Esc quits; fps in the title/console.
D3DPT\   -> paravirtual Direct3D 9 (our device, doc 14): D3D9.DLL next to
            D3D9TEST.EXE / D3DGAME9.EXE; needs the WIN2KXP (FXPTL.SYS) or
            WIN9X step like OPENGL32.DLL. Log: d3dpt.log next to the EXE
            (C:\d3dpt.log when run from the CD). Do not mix with WINED3D.
WINED3D\ -> the full WineD3D set (wine9x ${WINE9X_REF:0:7}) incl. the
            system-wide switcher DLLs; see WINE9X.TXT before touching system32.

Not included: GLIDE2X.OVL (DOS Glide games; needs Open Watcom to build).
TXT
# 8.3-safe upper-case names for Win9x
( cd "$OUT/iso" && for f in WIN9X/* WIN2KXP/* GAMEDIR/* WINED3D/* D3DPT/*; do
    u="$(dirname "$f")/$(basename "$f" | tr a-z A-Z)"; [ "$f" = "$u" ] || mv "$f" "$u"; done )

ISO="$OUT/guest-tools-3dfx-$REV.iso"
if command -v xorriso >/dev/null; then
  xorriso -as mkisofs -o "$ISO" -V "GUESTTOOLS" -J -r "$OUT/iso" >/dev/null 2>&1
elif command -v genisoimage >/dev/null; then
  genisoimage -o "$ISO" -V "GUESTTOOLS" -J -r "$OUT/iso" >/dev/null 2>&1
elif command -v mkisofs >/dev/null; then
  mkisofs -o "$ISO" -V "GUESTTOOLS" -J -r "$OUT/iso" >/dev/null 2>&1
else
  echo "no ISO tool found; staged files are in $OUT/iso"; exit 0
fi
echo "==> $ISO"
