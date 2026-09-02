#!/usr/bin/env bash
# Configure the prepared QEMU tree with a uv-managed Python, so the build
# never depends on whichever interpreter wins the host PATH race.
# Python version pinned in .python-version (QEMU 9.2.x supports <= 3.13;
# host 3.14s broke mkvenv/distlib).
#
# Usage: scripts/configure-qemu.sh [extra configure flags...]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/qemu"

command -v uv >/dev/null || { echo "uv not found — install it (https://docs.astral.sh/uv/)"; exit 1; }

PYVER="$(cat "$ROOT/.python-version")"
uv python install "$PYVER" --quiet
PYTHON="$(uv python find "$PYVER")"
echo "==> python: $PYTHON"

mkdir -p "$BUILD"
cd "$BUILD"
# --disable-werror: pinned 9.2.x trips new-toolchain warnings (glibc const strstr)
# --extra-cflags: vendored Khronos GL headers (third_party/khronos/README.md)
EXTRA_CFLAGS="-I$ROOT/third_party/khronos"
if [ "$(uname -s)" = Darwin ]; then
  # qemu-3dfx's Darwin path is GLX via XQuartz (patched meson.build hardcodes
  # /opt/X11) and the patch requires SDL2.
  [ -d /opt/X11/include ] || { echo "XQuartz missing: brew install --cask xquartz"; exit 1; }
  pkg-config --exists sdl2 || { echo "SDL2 missing: brew install sdl2"; exit 1; }
fi
"$ROOT/qemu/configure" \
  --python="$PYTHON" \
  --disable-werror \
  --extra-cflags="$EXTRA_CFLAGS" \
  --target-list=i386-softmmu,x86_64-softmmu \
  "$@"
