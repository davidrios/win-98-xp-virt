#!/usr/bin/env bash
# Run a command inside the Windows cross-build container
# (packaging/windows/Dockerfile, docs/build-windows.md).
#
#   scripts/win-cross.sh --build             build or refresh the image
#   scripts/win-cross.sh <command...>        run it in the checkout
#   scripts/win-cross.sh                     an interactive shell
#
# The checkout is bind-mounted at the same absolute path it has on the
# host, so every build directory the container writes (build/win/...,
# target/x86_64-pc-windows-gnu/...) is a real path afterwards and meson's
# absolute paths keep working from the host side too. Rootless podman with
# --userns=keep-id means those files come out owned by you.
#
# Everything Windows goes under build/win/ and target/x86_64-pc-windows-gnu/,
# never over the native build/qemu or target/release: a checkout can hold a
# Linux build and a Windows build at once, which is the whole point of
# cross-building rather than switching a tree back and forth.
#
# CONTAINER=docker uses docker instead of podman (then the files come out
# owned by root unless the daemon is rootless -- podman is the tested path).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${WIN_CROSS_IMAGE:-2ksbox-win-cross}"
FEDORA="${WIN_CROSS_FEDORA:-43}"
ENGINE="${CONTAINER:-podman}"

command -v "$ENGINE" >/dev/null || {
  echo "win-cross.sh: $ENGINE not found (install podman, or CONTAINER=docker)" >&2; exit 1; }

build_image() {
  echo "==> $ENGINE build $IMAGE (fedora:$FEDORA)"
  "$ENGINE" build --build-arg "FEDORA=$FEDORA" -t "$IMAGE" "$ROOT/packaging/windows"
}

if [ "${1:-}" = "--build" ]; then build_image; exit 0; fi

if ! "$ENGINE" image exists "$IMAGE" 2>/dev/null && \
   ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "==> $IMAGE is not built yet"
  build_image
fi

opts=(--rm -v "$ROOT:$ROOT" -w "$ROOT" -e "HOME=$ROOT/build/win/home")
[ "$ENGINE" = podman ] && opts+=(--userns=keep-id)
# A tty only when we have one: with no arguments this is a shell, and in a
# script (or under a test harness) there is nothing to attach to.
if [ -t 0 ] && [ -t 1 ]; then opts+=(-it); fi
# Pass through the knobs the build scripts read, so `WIN_STAGES=qemu
# scripts/build-windows.sh` works from outside the container too.
for v in JOBS WIN_STAGES WIN_SKIP_TEST QEMU_PYTHON CARGO_BUILD_JOBS QEMU_EMBED_LIB_DIR; do
  [ -n "${!v:-}" ] && opts+=(-e "$v=${!v}")
done

mkdir -p "$ROOT/build/win/home"
if [ $# -eq 0 ]; then set -- /bin/bash; fi
exec "$ENGINE" run "${opts[@]}" "$IMAGE" "$@"
