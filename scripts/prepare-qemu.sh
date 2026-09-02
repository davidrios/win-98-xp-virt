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

echo "==> overlaying hw/3dfx and hw/mesa"
# -c: checksum compare so unchanged files (esp. meson.build) are not rewritten —
# a touched meson.build makes ninja regenerate and reset configure options.
rsync -rc "$FX/qemu-0/hw/3dfx" "$FX/qemu-1/hw/mesa" "$QEMU/hw/"

# Applied-check: the patch wires glidept_mm_init() into pc.c; sign_commit
# later edits vl.c, so a `git apply --reverse --check` would wrongly fail.
if grep -q glidept_mm_init "$QEMU/hw/i386/pc.c"; then
  echo "==> patch already applied"
else
  echo "==> applying $(basename "$PATCH")"
  git -C "$QEMU" apply "$PATCH"
fi

echo "==> applying our patch queue (patches/qemu/*.patch)"
for p in "$ROOT"/patches/qemu/*.patch; do
  [ -e "$p" ] || continue
  if git -C "$QEMU" apply --reverse --check "$p" 2>/dev/null; then
    echo "    $(basename "$p"): already applied"
  elif git -C "$QEMU" apply --check "$p" 2>/dev/null; then
    git -C "$QEMU" apply "$p" && echo "    $(basename "$p"): applied"
  else
    echo "    $(basename "$p"): DOES NOT APPLY"; exit 1
  fi
done

echo "==> signing with qemu-3dfx commit"
(cd "$QEMU" && bash "$FX/scripts/sign_commit" -git="$FX")

echo "==> done. Configure with: scripts/configure-qemu.sh  (uv-managed python, --disable-werror)"
