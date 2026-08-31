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
"$ROOT/qemu/configure" \
  --python="$PYTHON" \
  --disable-werror \
  --target-list=i386-softmmu,x86_64-softmmu \
  "$@"
