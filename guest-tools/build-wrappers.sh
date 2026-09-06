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
  # (MMX-register forms of punpck* are Pentium MMX; only the %xmm forms are SSE2)
  local n; n=$(objdump -d "$1" | grep -v '%mm[0-7]' | grep -cE '\b(movdq[au]|movapd|movupd|pshufd|punpck[hl](bw|wd|dq|qdq)|paddq|cvtsd2|cvtsi2sd|cvttsd2si|movsd[[:space:]]+%xmm|xorpd|andpd|popcnt|ptest|pcmpistr|pshufb|pmaddubsw)\b' || true)
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

# The ISO: one folder per role, one copy of every file. What used to be
# GAMEDIR\ was three different stacks in one folder — the WineD3D DLLs
# under the same names ours use, next to the test EXEs — so a "copy this
# next to the game" instruction could silently give you the wrong D3D.
# Now each stack owns a folder, every test program lives in TESTS\, and
# SETUP.EXE does the copying (including WineD3D's renames, which is why
# the disc no longer carries a second copy of those DLLs).
rm -rf "$OUT/iso"
mkdir -p "$OUT/iso"/{GLIDE,OPENGL,D3DPT,WINED3D,TESTS,CDSHELF}
G="$FX/wrappers/3dfx/build"; M="$FX/wrappers/mesa/build"
T="$OUT/iso/TESTS"

# GLIDE\: the device mapper (FXMEMMAP.VXD on 9x, FXPTL.SYS + INSTDRV on
# NT) and the Glide wrappers. One folder for both families: the DLLs are
# the same files, only the directory they are copied into differs, and
# SETUP.EXE knows which is which.
cp "$G"/glide.dll "$G"/glide2x.dll "$G"/glide3x.dll "$G"/fxmemmap.vxd \
   "$G"/fxptl.sys "$G"/instdrv.exe "$OUT/iso/GLIDE/"
# OPENGL\: the GL pass-through wrapper, per game.
cp "$M"/opengl32.dll "$OUT/iso/OPENGL/"
# WINED3D\: the wine9x set under wine9x's own names, once. A per-game
# install is WINED3D.DLL + one interface renamed (WINED9 -> D3D9); the
# switchers are the system-wide variant, see WINE9X.TXT.
W="$OUT/wine9x"
cp "$W"/wined3d.dll "$W"/winedd.dll "$W"/wined8.dll "$W"/wined9.dll \
   "$W"/ddraw_xp.dll "$W"/d3d8_xp.dll "$W"/d3d9_xp.dll \
   "$W"/ddraw_98.dll "$W"/d3d8_98.dll "$W"/d3d9_98.dll "$OUT/iso/WINED3D/"
cp "$W"/README.md "$OUT/iso/WINED3D/WINE9X.TXT"

# D3DPT\: Direct3D 8/9 over our paravirtual device (doc 14), with
# qemu-3dfx's fxlib device mapper (FXPTL.SYS / FXMEMMAP.VXD). Per game:
# only the DLLs live here, so nothing in this folder can be confused with
# the WineD3D set. d3d9_vtbl.h is generated from mingw's d3d9.h
# (gen_vtbl.py) and checked in.
i686-w64-mingw32-gcc -O2 -Wall -shared -o "$OUT/iso/D3DPT/d3d9.dll" "$ROOT/guest-tools/src/d3dpt/d3d9.c" \
  "$FX/wrappers/fxlib/fxlibnt.c" "$FX/wrappers/fxlib/fxlib9x.c" -I"$FX/wrappers/fxlib" \
  -static-libgcc -Wl,--kill-at -lgdi32 -luser32 -lpsapi
# Direct3D 8 over the same device (doc 14 P4): d3d8.c includes d3d9.c, one DLL.
i686-w64-mingw32-gcc -O2 -Wall -shared -o "$OUT/iso/D3DPT/d3d8.dll" "$ROOT/guest-tools/src/d3dpt/d3d8.c" \
  "$FX/wrappers/fxlib/fxlibnt.c" "$FX/wrappers/fxlib/fxlib9x.c" -I"$FX/wrappers/fxlib" \
  -static-libgcc -Wl,--kill-at -lgdi32 -luser32 -lpsapi
# DirectDraw 7 shim (d3dpt/ddraw.c): forwards to the system ddraw.dll and
# reports 256 MB of video memory. RenderWare launchers (GTA Vice City) ask
# DirectDraw, not Direct3D, and refuse the Cirrus adapter's 4 MB.
i686-w64-mingw32-gcc -O2 -Wall -shared -o "$OUT/iso/D3DPT/ddraw.dll" "$ROOT/guest-tools/src/d3dpt/ddraw.c" \
  "$ROOT/guest-tools/src/d3dpt/ddraw.def" -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup
# DirectInput shim (d3dpt/dinput.c): forwards to the system dinput.dll and
# merges GetAsyncKeyState into a non-exclusive keyboard's state — the fix for
# a game whose loop stops pumping messages (FIFA 2000's match, doc 15).
# Silent by default; D3DPT_DINPUT_LOG=1 in the environment adds the log of
# what the game asks of its keyboard / mouse devices and what it gets back.
i686-w64-mingw32-gcc -O2 -Wall -shared -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
  -o "$OUT/iso/D3DPT/dinput.dll" "$ROOT/guest-tools/src/d3dpt/dinput.c" "$ROOT/guest-tools/src/d3dpt/dinput.def" \
  -static-libgcc -Wl,--kill-at -Wl,--enable-stdcall-fixup -ldxguid

# TESTS\: every test, benchmark and calibration program, one copy each.
# Which stack a test runs on is decided by what is copied next to it, not
# by which folder it came from — SETUP.EXE's /GAME does that.
# Reference workloads (doc 14 P0a): the same deterministic game-like scene on
# Direct3D 9 and Direct3D 8; -frames N -dump N x.bmp for golden images.
i686-w64-mingw32-gcc -O2 -o "$T/d3dgame9.exe" "$ROOT/guest-tools/src/d3dgame9.c" -ld3d9 -lgdi32 -luser32
i686-w64-mingw32-gcc -O2 -o "$T/d3dgame8.exe" "$ROOT/guest-tools/src/d3dgame8.c" -ld3d8 -lgdi32 -luser32
# Feature test (doc 14 P3): shaders, declarations, state blocks, queries, cube maps, surfaces.
i686-w64-mingw32-gcc -O2 -o "$T/d3dfeat9.exe" "$ROOT/guest-tools/src/d3dfeat9.c" -ld3d9 -lgdi32 -luser32
# D3D9 smoke test (guest-tools/src/d3d9test.c): adapter string, HAL caps,
# x87 control word after CreateDevice, spinning triangle with fps.
i686-w64-mingw32-gcc -O2 -o "$T/d3d9test.exe" "$ROOT/guest-tools/src/d3d9test.c" -ld3d9 -lgdi32 -luser32
# DDVMTEST.EXE prints what a launcher's video-memory check sees (the
# system ddraw against D3DPT\DDRAW.DLL).
i686-w64-mingw32-gcc -O2 -o "$T/ddvmtest.exe" "$ROOT/guest-tools/src/ddvmtest.c" -lddraw -ldxguid
# Display-mode probe (guest-tools/src/modetest.c): current mode, mode list,
# ChangeDisplaySettingsEx results for the switches ddraw/wined3d make.
i686-w64-mingw32-gcc -O2 -o "$T/modetest.exe" "$ROOT/guest-tools/src/modetest.c" -luser32
# GL smoke test: Mesa's wglgears, ships in qemu-3dfx's demos. Run it next to
# OPENGL32.DLL inside the guest; the title/console shows the renderer.
i686-w64-mingw32-gcc -O2 -o "$T/wglgears.exe" "$FX/wrappers/mesa/demos/wglgears.c" \
  -lopengl32 -lgdi32 -lglu32 -mwindows
# SSE throughput (guest-tools/src/ssebench.c, doc 16): D3DX-shaped SSE1
# kernels plus the same math in x87 C; ns per op, console + ssebench.log.
i686-w64-mingw32-gcc -O2 -o "$T/ssebench.exe" "$ROOT/guest-tools/src/ssebench.c"
# CDTEST.EXE: CD audio through MCI (doc 17 §6.3), the CD-ROM backend's in-guest check
i686-w64-mingw32-gcc -O2 -o "$T/cdtest.exe" "$ROOT/guest-tools/src/cdtest.c" -lwinmm
# CRT calibration patterns (doc 09, guest-tools/src/crtcal.c + crtcal.h): the
# same patterns tools/crtcal-render writes for the shader side, put on a real
# tube at the exact mode through an exclusive full-screen DirectDraw primary.
i686-w64-mingw32-gcc -O2 -o "$T/crtcal.exe" "$ROOT/guest-tools/src/crtcal.c" \
  -lddraw -ldxguid -luser32
# The 720x400 text-mode patterns (doc 09, guest-tools/src/textcal.asm). DOS
# only: 720x400 is the VGA *text* mode, which no Windows display driver
# offers, so it is reachable from FreeDOS or a "Restart in MS-DOS mode"
# screen and nowhere else.
nasm -f bin -o "$T/textcal.com" "$ROOT/guest-tools/src/textcal.asm"

# CDSHELF: the host's disc shelf from inside the machine (doc 07, patch 52;
# protocol cdshelf/cdshelf_proto.h). One EXE for both Windows families — SPTI
# on XP, WNASPI32 loaded at run time on Win98 — plus a DOS .COM that drives
# the drive by PIO, for a DOS box that has neither. -mwindows: run with no
# arguments it is a window (a disc swap is a thing you do, not a command line
# you retype); its verbs still write to a redirected stdout.
i686-w64-mingw32-gcc -O2 -Wall -mwindows -o "$OUT/iso/CDSHELF/cdshelf.exe" \
  "$ROOT/guest-tools/src/cdshelf.c" -I"$ROOT/cdshelf"
nasm -f bin -o "$OUT/iso/CDSHELF/cdshelf.com" "$ROOT/guest-tools/src/cdshelf.asm"

# XP display driver for the d3dpt-vga adapter (doc 15, M7a): built and
# checked by its own script (kernel-mode PE rules differ), staged as DRIVER\.
"$ROOT/guest-tools/build-driver.sh" >/dev/null
mkdir -p "$OUT/iso/DRIVER" && cp "$ROOT"/guest-tools/out/driver/* "$OUT/iso/DRIVER/"

# SETUP.EXE at the root: the installer that reads the folders above and
# knows which of them this guest's Windows wants (guest-tools/src/setup.c).
i686-w64-mingw32-gcc -O2 -Wall -o "$OUT/iso/setup.exe" "$ROOT/guest-tools/src/setup.c" \
  -ladvapi32 -luser32

for f in "$OUT"/iso/*/*.dll "$OUT"/iso/*/*.exe "$OUT"/iso/*.exe; do check_crt "$f"; check_isa "$f"; done
# CRLF: Win9x Notepad shows LF-only text as one line
crlf() { awk '{ sub(/\r$/, ""); printf "%s\r\n", $0 }'; }
sed -e "s/@REV@/$REV/" -e "s/@WINE9X@/${WINE9X_REF:0:7}/" "$ROOT/guest-tools/README-ISO.txt" \
  | crlf > "$OUT/iso/README.TXT"
# 8.3-safe upper-case names for Win9x
( cd "$OUT/iso" && for f in */* *.exe; do
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
