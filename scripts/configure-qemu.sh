#!/usr/bin/env bash
# Configure the prepared QEMU tree with a uv-managed Python, so the build
# never depends on whichever interpreter wins the host PATH race.
# Python version pinned in .python-version (QEMU 9.2.x supports <= 3.13;
# host 3.14s broke mkvenv/distlib).
#
# Usage: scripts/configure-qemu.sh [--windows] [extra configure flags...]
#
# --windows cross-compiles for Windows x86_64 with mingw-w64 into
# build/win/qemu instead of build/qemu, against the Rust staticlib built
# for x86_64-pc-windows-gnu. Run it inside the cross container
# (scripts/win-cross.sh), which is where the mingw glib/pixman/epoxy the
# build needs actually exist — docs/build-windows.md. The two build
# directories are independent, so one checkout holds a Linux build and a
# Windows build at once.
#
# QEMU_PYTHON=<interpreter> uses that one and never consults uv — for a
# build inside a sandbox that has a suitable Python already and cannot
# fetch one (the Flatpak, M6 step 6b: no uv in org.freedesktop.Sdk, and no
# network during the build). It is checked for version rather than
# trusted, because the failure it prevents (3.14 breaking mkvenv) is
# obscure at the point it bites.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

WINDOWS=""
if [ "${1:-}" = "--windows" ]; then WINDOWS=1; shift; fi

if [ -n "$WINDOWS" ]; then
  BUILD="$ROOT/build/win/qemu"
  CARGO_TARGET=x86_64-pc-windows-gnu
else
  BUILD="$ROOT/build/qemu"
  CARGO_TARGET=""
fi
LIBDISC_DIR="$ROOT/target${CARGO_TARGET:+/$CARGO_TARGET}/release"

PYVER="$(cat "$ROOT/.python-version")"
if [ -n "${QEMU_PYTHON:-}" ]; then
  PYTHON="$QEMU_PYTHON"
  command -v "$PYTHON" >/dev/null || { echo "QEMU_PYTHON=$PYTHON is not executable"; exit 1; }
  # QEMU 9.2.x supports 3.8 … 3.13; anything newer breaks mkvenv/distlib.
  "$PYTHON" -c 'import sys; sys.exit(0 if (3,8) <= sys.version_info[:2] <= (3,13) else 1)' || {
    echo "QEMU_PYTHON=$PYTHON is $("$PYTHON" -V 2>&1); QEMU 9.2.x needs 3.8–3.13"; exit 1; }
else
  command -v uv >/dev/null || {
    echo "uv not found — install it (https://docs.astral.sh/uv/), or set QEMU_PYTHON to a 3.8–3.13 interpreter"; exit 1; }
  uv python install "$PYVER" --quiet
  PYTHON="$(uv python find "$PYVER")"
fi
echo "==> python: $PYTHON ($("$PYTHON" -V 2>&1))"

# libdisc (the CD-ROM image model, libdisc/): a Rust staticlib linked into
# qemu-system-* and libqemu-embed-* for block/cdimage.c (patch 50). The crate
# has no QEMU dependency, so no cycle with the player.
echo "==> cargo build --release -p libdisc${CARGO_TARGET:+ --target $CARGO_TARGET}"
(cd "$ROOT" && cargo build --release -p libdisc ${CARGO_TARGET:+--target "$CARGO_TARGET"})

mkdir -p "$BUILD"
cd "$BUILD"
# --disable-werror: pinned 9.2.x trips new-toolchain warnings (glibc const strstr)
# --extra-cflags: vendored Khronos GL headers (third_party/khronos/README.md)
# -fPIC: objects are also linked into libqemu-embed-<target> (shared)
EXTRA_CFLAGS="-I$ROOT/third_party/khronos -fPIC"
CFG=(-Db_staticpic=true)
if [ -n "$WINDOWS" ]; then
  # PE code is position-independent by construction and gcc says so on every
  # file it compiles ("-fPIC ignored for target"), so the native build's flag
  # goes away here rather than being repeated a few thousand times.
  EXTRA_CFLAGS="-I$ROOT/third_party/khronos"
  CFG=(--cross-prefix=x86_64-w64-mingw32-)
  command -v x86_64-w64-mingw32-gcc >/dev/null || {
    echo "no x86_64-w64-mingw32-gcc — run this inside scripts/win-cross.sh"; exit 1; }
elif [ "$(uname -s)" = Darwin ]; then
  # qemu-3dfx's Darwin path is GLX via XQuartz (patched meson.build hardcodes
  # /opt/X11) and the patch requires SDL2.
  [ -d /opt/X11/include ] || { echo "XQuartz missing: brew install --cask xquartz"; exit 1; }
  pkg-config --exists sdl2 || { echo "SDL2 missing: brew install sdl2"; exit 1; }
  # SDK 15.4+ declares strchrnul (and friends) with an availability of 15.4;
  # QEMU detects and uses them unguarded, so a lower deployment target spams
  # -Wunguarded-availability-new. Target the running OS for local builds
  # (release packaging picks its own floor). Honour a preset value.
  if [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
    export MACOSX_DEPLOYMENT_TARGET="$(sw_vers -productVersion | cut -d. -f1,2)"
  fi
  echo "==> MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
fi
"$ROOT/qemu/configure" \
  --python="$PYTHON" \
  --disable-werror \
  --extra-cflags="$EXTRA_CFLAGS" \
  "${CFG[@]}" \
  --target-list=i386-softmmu,x86_64-softmmu \
  -Dlibdisc_dir="$LIBDISC_DIR" \
  "$@"

# QEMU's configure writes `werror = true` into its native file for git
# checkouts on Linux/Windows; meson re-applies native-file options on
# auto-regeneration, overriding the -Dwerror=false from --disable-werror and
# breaking the pinned 9.2.x build on new toolchains. Strip it.
sed -i.bak '/^werror = true$/d' "$BUILD/config-meson.cross" && rm -f "$BUILD/config-meson.cross.bak"
# keep the edited native file from looking newer than build.ninja (spurious regen)
touch -r "$BUILD/build.ninja" "$BUILD/config-meson.cross"
