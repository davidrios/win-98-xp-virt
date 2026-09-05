#!/usr/bin/env bash
# The Linux package (M6 step 6, doc 07's install layout): stage everything
# a stranger needs into one relocatable tree, check that the staged
# launcher really resolves its companions *inside* it, and roll a tarball.
#
#   scripts/package-linux.sh                 # build, stage, check, tar
#   scripts/package-linux.sh --no-build      # use target/release as it is
#   scripts/package-linux.sh --no-tar        # leave the staged tree only
#   scripts/package-linux.sh --with-shaders  # include the preset collection
#   scripts/package-linux.sh --out DIR       # default build/package
#
# It does not build QEMU: build/qemu (libqemu-embed-i386.so + qemu-img)
# and qemu/pc-bios must already be there, per CLAUDE.md's build order.
# The guest-tools ISO is included when guest-tools/out has one.
#
# The layout, relative to the tree's root (= an install prefix):
#   bin/win98-xp-virt                  the launcher
#   bin/win98-xp-virt-player           the player
#   lib/win98-xp-virt/libqemu-embed-i386.so
#   libexec/win98-xp-virt/qemu-img     ours, patched — kept off PATH
#   share/win98-xp-virt/pc-bios/       QEMU firmware (the player's -L)
#   share/win98-xp-virt/guest-tools/   the guest-tools ISO
#   share/win98-xp-virt/shaders/       presets, with --with-shaders
#   share/win98-xp-virt/desktop/       .desktop + icon, for install.sh
#   share/doc/win98-xp-virt/           COPYING, notices, README
#   install.sh                         copy the above into a prefix
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD=1 TAR=1 SHADERS=0 OUT="$ROOT/build/package"
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0; shift ;;
    --no-tar) TAR=0; shift ;;
    --with-shaders) SHADERS=1; shift ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "package-linux.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
NAME="win98-xp-virt-$VERSION-linux-$(uname -m)"
STAGE="$OUT/$NAME"

need() { [ -e "$1" ] || { echo "package-linux.sh: missing $1${2:+ ($2)}" >&2; exit 1; }; }
need build/qemu/libqemu-embed-i386.so "scripts/configure-qemu.sh && ninja -C build/qemu libqemu-embed-i386.so"
need build/qemu/qemu-img "ninja -C build/qemu qemu-img"
need qemu/pc-bios "scripts/prepare-qemu.sh"

if [ "$BUILD" = 1 ]; then
  cargo build --release -p launcher -p player
fi
need target/release/launcher
need target/release/player

rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,lib/win98-xp-virt,libexec/win98-xp-virt,share/win98-xp-virt/desktop,share/doc/win98-xp-virt}

install -m755 target/release/launcher "$STAGE/bin/win98-xp-virt"
install -m755 target/release/player "$STAGE/bin/win98-xp-virt-player"
install -m755 build/qemu/libqemu-embed-i386.so "$STAGE/lib/win98-xp-virt/"
install -m755 build/qemu/qemu-img "$STAGE/libexec/win98-xp-virt/"
cp -a qemu/pc-bios "$STAGE/share/win98-xp-virt/pc-bios"

# The guest-tools ISO: the newest one, the same choice the launcher's
# "Add guest-tools ISO" button makes in a checkout.
iso=$(ls -t guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1 || true)
if [ -n "$iso" ]; then
  mkdir -p "$STAGE/share/win98-xp-virt/guest-tools"
  install -m644 "$iso" "$STAGE/share/win98-xp-virt/guest-tools/"
else
  echo "package-linux.sh: no guest-tools ISO in guest-tools/out (guest-tools/build-wrappers.sh); packaging without it"
fi

# The shader presets are 80 MB and the launcher can fetch them itself, so
# they are opt-in; a distro package that would rather ship them says so.
if [ "$SHADERS" = 1 ]; then
  need third_party/slang-shaders "git submodule update --init third_party/slang-shaders"
  mkdir -p "$STAGE/share/win98-xp-virt/shaders"
  # No .git*: this is a copy of the presets, not a checkout of them.
  tar -c --exclude='.git*' -C third_party/slang-shaders . | tar -x -C "$STAGE/share/win98-xp-virt/shaders"
fi

install -m644 packaging/linux/win98-xp-virt.desktop "$STAGE/share/win98-xp-virt/desktop/"
install -m644 packaging/linux/win98-xp-virt.svg "$STAGE/share/win98-xp-virt/desktop/"
install -m755 packaging/linux/install.sh "$STAGE/install.sh"
install -m644 COPYING THIRD-PARTY-NOTICES.md README.md "$STAGE/share/doc/win98-xp-virt/"

# --- the check -------------------------------------------------------
# A package whose launcher still answers with the checkout it was built
# from is not a package. Ask the staged binary itself, with `env -i` so
# not one LAUNCHER_*/PLAYER_* knob from this shell can be what makes it
# work, and from `/` so nothing is found by a relative path either.
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
resolved=$(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
  "$STAGE/bin/win98-xp-virt" --paths)
echo "$resolved"
fail=0
while read -r what path; do
  case "$what" in
    player|qemu-img|pc-bios|guest-tools|prefix) ;;
    *) continue ;;
  esac
  # `--paths` says "(none built or shipped)" where there is nothing to
  # name — a package rolled without a guest-tools ISO, which is allowed
  # and already warned about above.
  case "$path" in "("*) continue ;; esac
  case "$path" in
    "$STAGE"|"$STAGE"/*) ;;
    *) echo "package-linux.sh: $what resolved outside the package: $path" >&2; fail=1 ;;
  esac
done <<< "$resolved"
# The player's own dependency: an installed tree has no build/qemu, so the
# origin-relative rpath (player/build.rs) is what has to find the embed
# library. ldd resolves it exactly as the loader will.
embed=$(cd / && env -i ldd "$STAGE/bin/win98-xp-virt-player" | sed -n 's/.*libqemu-embed-i386.so => \([^ ]*\).*/\1/p')
embed=$(readlink -f "$embed" 2>/dev/null || echo "$embed")  # the loader reports it via bin/../lib
case "$embed" in
  "$STAGE"/lib/win98-xp-virt/*) echo "libqemu-embed  $embed" ;;
  *) echo "package-linux.sh: the player's libqemu-embed came from $embed, not the package" >&2; fail=1 ;;
esac
# The machine's own bundle-creating path, end to end: the staged launcher
# runs the staged qemu-img to make a disk, and translates the result to a
# command line pointing at the staged firmware.
(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
  "$STAGE/bin/win98-xp-virt" --wizard-new xp "Package check" 1 >/dev/null)
bundle="$scratch/machines/package-check/machine.toml"
args=$(cd / && env -i HOME="$scratch" "$STAGE/bin/win98-xp-virt" --print-args "$bundle")
case "$args" in
  *"-L $STAGE/share/win98-xp-virt/pc-bios"*) echo "qemu args      -L inside the package" ;;
  *) echo "package-linux.sh: --print-args did not point at the packaged firmware" >&2; fail=1 ;;
esac
[ -s "$scratch/machines/package-check/disk.qcow2" ] \
  || { echo "package-linux.sh: the packaged qemu-img did not create a disk" >&2; fail=1; }
desktop-file-validate "$STAGE/share/win98-xp-virt/desktop/win98-xp-virt.desktop" \
  || { echo "package-linux.sh: the desktop entry is not valid" >&2; fail=1; }
[ "$fail" = 0 ] || exit 1
echo "checks passed"

du -sh "$STAGE" | sed 's/^/staged  /'
if [ "$TAR" = 1 ]; then
  if tar --help 2>/dev/null | grep -q -- --zstd; then
    archive="$OUT/$NAME.tar.zst"; comp=(--zstd)
  else
    archive="$OUT/$NAME.tar.gz"; comp=(-z)
  fi
  rm -f "$archive"
  tar -C "$OUT" "${comp[@]}" -cf "$archive" "$NAME"
  du -h "$archive" | sed 's/^/tarball /'
fi
echo "package: $STAGE"
