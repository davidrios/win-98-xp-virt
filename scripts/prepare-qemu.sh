#!/usr/bin/env bash
# Prepare the QEMU submodule tree: overlay qemu-3dfx device models, apply the
# version-matched patch, and stamp the qemu-3dfx commit id (guest wrappers
# verify it — build wrappers from the SAME third_party/qemu-3dfx commit).
#
# Idempotent. Reset with:  git -C qemu checkout . && git -C qemu clean -fd hw/3dfx hw/mesa
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# qemu-3dfx's sign_commit uses GNU `sed -i -e`; BSD sed would leave "-e"
# backup files behind. Prefer Homebrew gnu-sed on macOS.
if [ "$(uname -s)" = Darwin ]; then
  GNUBIN="$(brew --prefix gnu-sed 2>/dev/null || true)/libexec/gnubin"
  [ -d "$GNUBIN" ] && export PATH="$GNUBIN:$PATH" || echo "warning: gnu-sed not found (brew install gnu-sed)"
fi
QEMU="$ROOT/qemu"
FX="$ROOT/third_party/qemu-3dfx"
PATCH="$FX/00-qemu92x-mesa-glide.patch"

[ -f "$QEMU/VERSION" ] || { echo "qemu submodule missing (git submodule update --init)"; exit 1; }
[ -f "$PATCH" ] || { echo "qemu-3dfx submodule missing (git submodule update --init)"; exit 1; }

case "$(cat "$QEMU/VERSION")" in
  9.2.*) ;;
  *) echo "QEMU $(cat "$QEMU/VERSION") is not 9.2.x — patch/version mismatch"; exit 1 ;;
esac

# qemu-3dfx's sign_commit rewrites hw/{3dfx,mesa}/meson.build with sed -i on
# every run; a fresh mtime makes ninja regenerate the build. Snapshot the
# meson files and restore their mtimes when the content is unchanged.
SNAP="$(mktemp -d)"; trap 'rm -rf "$SNAP"' EXIT
for f in meson.build hw/3dfx/meson.build hw/mesa/meson.build; do
  [ -f "$QEMU/$f" ] && mkdir -p "$SNAP/$(dirname "$f")" && cp -p "$QEMU/$f" "$SNAP/$f"
done
restore_mtimes() {
  for f in meson.build hw/3dfx/meson.build hw/mesa/meson.build; do
    if [ -f "$SNAP/$f" ] && cmp -s "$SNAP/$f" "$QEMU/$f"; then
      touch -r "$SNAP/$f" "$QEMU/$f"
    fi
  done
}

echo "==> overlaying hw/3dfx and hw/mesa"
# -c: checksum compare so unchanged files (esp. meson.build) are not rewritten —
# a touched meson.build makes ninja regenerate and reset configure options.
rsync -rc "$FX/qemu-0/hw/3dfx" "$FX/qemu-1/hw/mesa" "$QEMU/hw/"

echo "==> overlaying embed/ (libqemu_embed)"
rsync -rc --delete "$ROOT/embed/" "$QEMU/embed/"

# Deterministic: restore every TRACKED file any patch touches to pristine
# v9.2.4, then apply the 3dfx patch and our queue fresh. (Overlay files were
# already refreshed by rsync above.) Partial states — e.g. a manual
# `git checkout meson.build` — previously slipped past an "already applied"
# heuristic and silently dropped hunks.
patched_files() {  # print paths from '+++ ./x' (diff -Nru) and '+++ b/x' (git) headers
  sed -n 's|^+++ \./||p; s|^+++ b/||p' "$@" | sort -u
}
echo "==> restoring tracked files touched by patches"
patched_files "$PATCH" "$ROOT"/patches/qemu/*.patch | while read -r f; do
  if git -C "$QEMU" ls-files --error-unmatch "$f" >/dev/null 2>&1; then
    git -C "$QEMU" checkout -q -- "$f"
  fi
done

echo "==> applying $(basename "$PATCH")"
git -C "$QEMU" apply "$PATCH"

echo "==> applying our patch queue (patches/qemu/*.patch)"
for p in "$ROOT"/patches/qemu/*.patch; do
  [ -e "$p" ] || continue
  if git -C "$QEMU" apply --check "$p" 2>/dev/null; then
    git -C "$QEMU" apply "$p" && echo "    $(basename "$p"): applied"
  else
    echo "    $(basename "$p"): DOES NOT APPLY"; exit 1
  fi
done

echo "==> signing with qemu-3dfx commit"
(cd "$QEMU" && bash "$FX/scripts/sign_commit" -git="$FX")

restore_mtimes
echo "==> done. Configure with: scripts/configure-qemu.sh  (uv-managed python, --disable-werror)"
