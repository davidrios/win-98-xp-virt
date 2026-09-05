#!/usr/bin/env bash
# Install this package into a prefix, or just tell you that you don't have
# to. The extracted tree is already relocatable — `bin/win98-xp-virt`
# finds the player, qemu-img, the firmware and the guest-tools ISO
# relative to its own location (doc 07's install layout) — so running it
# from wherever it was unpacked works. This script is for the rest: a
# desktop entry and an icon, so the launcher is in the applications menu.
#
#   ./install.sh                 # into ~/.local (no root)
#   ./install.sh --prefix /opt/win98-xp-virt
#   sudo ./install.sh --prefix /usr/local
#   ./install.sh --uninstall     # removes what a previous run put there
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
prefix=${HOME}/.local
uninstall=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix=$2; shift 2 ;;
    --prefix=*) prefix=${1#*=}; shift ;;
    --uninstall) uninstall=1; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "install.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

app=win98-xp-virt
desktop_dir=$prefix/share/applications
icon_dir=$prefix/share/icons/hicolor/scalable/apps

if [ "$uninstall" = 1 ]; then
  rm -rf "$prefix/lib/$app" "$prefix/libexec/$app" "$prefix/share/$app"
  rm -f "$prefix/bin/$app" "$prefix/bin/$app-player"
  rm -f "$desktop_dir/$app.desktop" "$icon_dir/$app.svg"
  command -v update-desktop-database >/dev/null && update-desktop-database "$desktop_dir" 2>/dev/null || true
  echo "removed $app from $prefix"
  exit 0
fi

if [ "$here" -ef "$prefix" ]; then
  echo "install.sh: source and destination are the same directory" >&2
  exit 1
fi

# The layout is copied wholesale: every path the launcher resolves is
# relative to bin/, so anything that arrives here as a set has to leave as
# one. `cp -a` rather than install(1) per file — pc-bios alone is hundreds
# of files, and none of them need a mode of their own.
for dir in bin lib libexec share; do
  [ -d "$here/$dir" ] || continue
  mkdir -p "$prefix/$dir"
  cp -a "$here/$dir/." "$prefix/$dir/"
done

# The desktop entry ships with a bare `Exec=win98-xp-virt`, which is only
# right if the prefix's bin/ is on PATH. It is here that we know the
# absolute path, so write it in.
mkdir -p "$desktop_dir" "$icon_dir"
sed -e "s|^Exec=.*|Exec=$prefix/bin/$app|" \
    -e "s|^Icon=.*|Icon=$icon_dir/$app.svg|" \
    "$here/share/$app/desktop/$app.desktop" > "$desktop_dir/$app.desktop"
cp -f "$here/share/$app/desktop/$app.svg" "$icon_dir/$app.svg"
command -v update-desktop-database >/dev/null && update-desktop-database "$desktop_dir" 2>/dev/null || true

echo "installed into $prefix"
echo "  launcher: $prefix/bin/$app"
echo "  player:   $prefix/bin/$app-player"
case ":$PATH:" in
  *":$prefix/bin:"*) ;;
  *) echo "  ($prefix/bin is not on your PATH; the desktop entry uses the absolute path)" ;;
esac
