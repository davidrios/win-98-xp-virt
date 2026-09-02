#!/usr/bin/env bash
# Build qemu-3dfx guest wrappers (Windows DLLs) from the SAME
# third_party/qemu-3dfx commit our QEMU fork is signed with, and stage them
# as a guest-tools ISO. Needs: i686-w64-mingw32-gcc, gendef, xxd, shasum,
# git, make; xorriso or genisoimage/mkisofs for the ISO.
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
ensure_msvcrt_cc() {
  local bin="$ROOT/guest-tools/tools/bin" real
  real="$(PATH="${PATH#"$bin:"}" command -v i686-w64-mingw32-gcc || true)"
  [ -n "$real" ] || { echo "need i686-w64-mingw32-gcc (mingw-w64)"; exit 1; }
  [ -f "$(dirname "$(dirname "$real")")/i686-w64-mingw32/lib/libmsvcrt-os.a" ] || \
  [ -f "$(dirname "$real")/../i686-w64-mingw32/lib/libmsvcrt-os.a" ] || \
    echo "warning: libmsvcrt-os.a not found next to the toolchain; msvcrt link may fail"
  printf '#!/usr/bin/env bash\nexec "%s" %s "$@"\n' "$real" "$MSVCRT_FLAGS" > "$bin/i686-w64-mingw32-gcc"
  chmod +x "$bin/i686-w64-mingw32-gcc"
}
ensure_msvcrt_cc

check_crt() {  # fail loudly if anything still imports the UCRT api-sets
  if objdump -p "$1" | grep -q 'api-ms-win-crt'; then
    echo "ERROR: $1 links against the UCRT (not loadable on Win9x)"; exit 1
  fi
}

build_wrapper() {  # $1 = 3dfx | mesa
  local d="$FX/wrappers/$1/build"
  rm -rf "$d" && mkdir -p "$d"
  ( cd "$d" && bash "$FX/scripts/conf_wrapper" >/dev/null && make )   # serial: 'fxlib' step must precede objects
}

echo "==> qemu-3dfx commit $REV"
build_wrapper 3dfx
build_wrapper mesa

rm -rf "$OUT" && mkdir -p "$OUT/iso/WIN9X" "$OUT/iso/WIN2KXP" "$OUT/iso/GAMEDIR"
G="$FX/wrappers/3dfx/build"; M="$FX/wrappers/mesa/build"
cp "$G"/glide.dll "$G"/glide2x.dll "$G"/glide3x.dll "$G"/fxmemmap.vxd "$OUT/iso/WIN9X/"
cp "$G"/glide.dll "$G"/glide2x.dll "$G"/glide3x.dll "$G"/fxptl.sys "$G"/instdrv.exe "$OUT/iso/WIN2KXP/"
cp "$M"/opengl32.dll "$OUT/iso/GAMEDIR/"
# GL smoke test: Mesa's wglgears, ships in qemu-3dfx's demos. Run it next to
# OPENGL32.DLL inside the guest; the title/console shows the renderer.
i686-w64-mingw32-gcc -O2 -o "$OUT/iso/GAMEDIR/wglgears.exe" "$FX/wrappers/mesa/demos/wglgears.c" \
  -lopengl32 -lgdi32 -lglu32 -mwindows
for f in "$OUT"/iso/*/*.dll "$OUT"/iso/*/*.exe; do check_crt "$f"; done
# CRLF: Win9x Notepad shows LF-only text as one line
crlf() { awk '{ sub(/\r$/, ""); printf "%s\r\n", $0 }'; }
crlf > "$OUT/iso/README.TXT" <<TXT
qemu-3dfx guest wrappers, built from qemu-3dfx commit $REV
(must match the host QEMU build's sign_commit stamp).

WIN9X\   -> copy GLIDE.DLL GLIDE2X.DLL GLIDE3X.DLL FXMEMMAP.VXD to C:\WINDOWS\SYSTEM
WIN2KXP\ -> copy GLIDE*.DLL to %SystemRoot%\system32, FXPTL.SYS to
            %SystemRoot%\system32\drivers, then run INSTDRV.EXE as Administrator
GAMEDIR\ -> copy OPENGL32.DLL next to each OpenGL game's EXE (Quake 2, etc.)
            WGLGEARS.EXE + OPENGL32.DLL in one folder = quick GL pass-through test

Not included: GLIDE2X.OVL (DOS Glide games; needs Open Watcom to build).
TXT
# 8.3-safe upper-case names for Win9x
( cd "$OUT/iso" && for f in WIN9X/* WIN2KXP/* GAMEDIR/*; do mv "$f" "$(dirname "$f")/$(basename "$f" | tr a-z A-Z)"; done )

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
