#!/usr/bin/env bash
# Prepare the OpenGLide submodule tree: restore every tracked file our patch
# queue touches, then apply patches/openglide/*.patch in filename order.
# Deterministic and idempotent, like prepare-qemu.sh and prepare-dxvk.sh.
# Never `git checkout` files inside third_party/openglide by hand between runs.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OG="$ROOT/third_party/openglide"

[ -f "$OG/Glide.cpp" ] || { echo "openglide submodule missing (git submodule update --init third_party/openglide)"; exit 1; }

# patches are plain `git diff` output (a/ b/ prefixes, applied with -p1)
patched_files() { sed -n 's|^+++ b/||p' "$@" | sort -u; }
created_files() { awk '/^--- \/dev\/null/ { getline; sub(/^\+\+\+ b\//, ""); print }' "$@" | sort -u; }

echo "==> restoring tracked files touched by patches/openglide"
patched_files "$ROOT"/patches/openglide/*.patch | while read -r f; do
  if git -C "$OG" ls-files --error-unmatch "$f" >/dev/null 2>&1; then
    git -C "$OG" checkout -q -- "$f"
  fi
done
created_files "$ROOT"/patches/openglide/*.patch | while read -r f; do rm -f "$OG/$f"; done

echo "==> applying our patch queue (patches/openglide/*.patch)"
for p in "$ROOT"/patches/openglide/*.patch; do
  echo "    $(basename "$p")"
  git -C "$OG" apply "$p"
done
echo "==> openglide tree ready ($(git -C "$OG" rev-parse --short HEAD) + $(ls "$ROOT"/patches/openglide/*.patch | wc -l | tr -d ' ') patches)"
echo "==> build with: scripts/build-glide.sh"
