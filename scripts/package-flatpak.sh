#!/usr/bin/env bash
# Build (and install) the Flatpak — M6 step 6b, manifest in
# packaging/flatpak/. Everything the build needs is compiled inside the
# SDK: the host's glibc is newer than the runtime's, so host-built
# binaries cannot run in it.
#
#   scripts/package-flatpak.sh              build, install --user, smoke check
#   scripts/package-flatpak.sh --no-install just build into the repo
#   scripts/package-flatpak.sh --check      only re-run the smoke check
#
# Environment:
#   FLATPAK_USER_DIR    which `--user` installation to use (flatpak's own
#                       variable). Set it if ~/.local/share/flatpak is on
#                       a full filesystem.
#   FLATPAK_BUILD_DIR   where flatpak-builder works, default build/flatpak.
#                       The build tree is several GB (a whole QEMU and a
#                       release Rust workspace), so point it at a disk
#                       with room.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

APPID=com._2ksbox.Launcher
MANIFEST="packaging/flatpak/$APPID.yml"
BUILD_DIR="${FLATPAK_BUILD_DIR:-$ROOT/build/flatpak}"
INSTALL=1 ONLY_CHECK=0
while [ $# -gt 0 ]; do
  case "$1" in
    --no-install) INSTALL=0; shift ;;
    --check) ONLY_CHECK=1; shift ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "package-flatpak.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

smoke() {
  # The app answering from inside its own sandbox: every companion has to
  # resolve under /app, which is the same property the tarball's check
  # makes, against a completely different prefix.
  echo "==> flatpak run $APPID --paths"
  local out
  out=$(flatpak run --user --command=2ksbox "$APPID" --paths) || return 1
  echo "$out"
  local fail=0
  while read -r what path; do
    case "$what" in player|qemu-img|pc-bios|guest-tools|prefix) ;; *) continue ;; esac
    case "$path" in "("*) continue ;; /app*) ;; *)
      echo "package-flatpak.sh: $what resolved outside /app: $path" >&2; fail=1 ;;
    esac
  done <<< "$out"
  # The data directory is the one thing a Flatpak deliberately moves: it
  # lands under ~/.var/app/<app-id>, not ~/.local/share.
  case "$out" in *"/.var/app/$APPID/"*) ;; *)
    echo "package-flatpak.sh: the library is not under ~/.var/app/$APPID" >&2; fail=1 ;;
  esac
  return $fail
}

if [ "$ONLY_CHECK" = 1 ]; then smoke; exit $?; fi

command -v flatpak-builder >/dev/null || { echo "flatpak-builder not installed" >&2; exit 1; }
# The patch queue runs on the host: it needs git and rsync, and the tree
# flatpak-builder copies has neither a .git nor rsync in the SDK.
[ -d qemu/hw/3dfx ] || { echo "qemu/ is not prepared: run scripts/prepare-qemu.sh first" >&2; exit 1; }
[ -f guest-tools/out/guest-tools-3dfx-*.iso ] 2>/dev/null || \
  ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1 || \
  echo "package-flatpak.sh: no guest-tools ISO built; the app will ship without it"

mkdir -p "$BUILD_DIR"
avail=$(df -Pk "$BUILD_DIR" | awk 'NR==2 {print int($4/1048576)}')
[ "$avail" -ge 12 ] || {
  echo "package-flatpak.sh: only ${avail} GB free at $BUILD_DIR; a QEMU + Rust build needs ~12 GB." >&2
  echo "Set FLATPAK_BUILD_DIR to somewhere with room." >&2; exit 1; }
echo "==> installation: ${FLATPAK_USER_DIR:-$HOME/.local/share/flatpak}"
echo "==> build dir:    $BUILD_DIR (${avail} GB free)"

args=(--user --force-clean --state-dir "$BUILD_DIR/state")
[ "$INSTALL" = 1 ] && args+=(--install)
flatpak-builder "${args[@]}" "$BUILD_DIR/build" "$MANIFEST"

[ "$INSTALL" = 1 ] || { echo "built (not installed): $BUILD_DIR/build"; exit 0; }
smoke
echo "installed: flatpak run $APPID"
