#!/usr/bin/env bash
# Prepare the QEMU submodule tree: overlay qemu-3dfx device models, apply the
# version-matched patch, and stamp the qemu-3dfx commit id (guest wrappers
# verify it — build wrappers from the SAME third_party/qemu-3dfx commit).
#
# Idempotent. Reset with:  git -C qemu checkout . && git -C qemu clean -fd hw/3dfx hw/mesa
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
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
rsync -r "$FX/qemu-0/hw/3dfx" "$FX/qemu-1/hw/mesa" "$QEMU/hw/"

if git -C "$QEMU" apply --reverse --check "$PATCH" 2>/dev/null; then
  echo "==> patch already applied"
else
  echo "==> applying $(basename "$PATCH")"
  git -C "$QEMU" apply "$PATCH"
fi

echo "==> signing with qemu-3dfx commit"
(cd "$QEMU" && bash "$FX/scripts/sign_commit" -git="$FX")

echo "==> done. Configure with e.g.:"
echo "    mkdir -p build/qemu && cd build/qemu && ../../qemu/configure --target-list=i386-softmmu,x86_64-softmmu"
