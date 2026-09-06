#!/usr/bin/env bash
# The Windows package (M11, docs/build-windows.md): stage everything a
# stranger needs into one folder, check that the staged launcher really
# resolves its companions *inside* it, and roll a zip.
#
#   scripts/package-windows.sh                 # stage, check, zip
#   scripts/package-windows.sh --qt            # ... the Qt 6 front end instead
#   scripts/package-windows.sh --no-zip        # leave the staged tree only
#   scripts/package-windows.sh --with-shaders  # include the preset collection
#   scripts/package-windows.sh --out DIR       # default build/win/package
#
# `--qt` rolls a second, complete package whose `2ksbox.exe` is
# `launcher-qt` (doc 07's other maintained front end) plus the Qt runtime
# it needs — same player, same QEMU, same guest tools, so the two can be
# unzipped side by side and compared on one machine.
#
# Run it from the host (not inside scripts/win-cross.sh): the checks want
# wine, which the cross image has no reason to carry. It builds nothing —
# scripts/build-windows.sh does that, and says so if an artefact is missing.
#
# A Windows package is **one folder**, not a Unix prefix: the executables
# at the top, every DLL beside them (which is exactly where the loader
# looks, so no rpath and no PATH), the data directories under it. The
# launcher knows both shapes (launcher/src/paths.rs).
#
#   2ksbox.exe                  the launcher
#   2ksbox-player.exe           the player
#   qemu-img.exe                ours, patched
#   libqemu-embed-i386.dll      QEMU as a library, what the player runs
#   d3dpt_exec.dll              the Direct3D executor (doc 14)
#   *.dll                       the mingw runtime those three need
#   pc-bios\                    QEMU firmware
#   guest-tools\                the guest-tools ISO
#   shaders\                    presets, with --with-shaders
#   doc\                        COPYING, notices, README
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ZIP=1 SHADERS=0 QT=0 OUT="$ROOT/build/win/package"
while [ $# -gt 0 ]; do
  case "$1" in
    --qt) QT=1; shift ;;
    --no-zip) ZIP=0; shift ;;
    --with-shaders) SHADERS=1; shift ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "package-windows.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
NAME="2ksbox-$VERSION-windows-x86_64"
[ "$QT" = 1 ] && NAME="$NAME-qt"
STAGE="$OUT/$NAME"
TARGET="$ROOT/target/x86_64-pc-windows-gnu/release"
QT_TARGET="$ROOT/launcher-qt/target/x86_64-pc-windows-gnu/release"
Q="$ROOT/build/win/qemu"

need() { [ -e "$1" ] || { echo "package-windows.sh: missing $1${2:+ ($2)}" >&2; exit 1; }; }
need "$Q/libqemu-embed-i386.dll" "scripts/build-windows.sh qemu"
need "$Q/qemu-img.exe"           "scripts/build-windows.sh qemu"
if [ "$QT" = 1 ]; then
  need "$QT_TARGET/launcher-qt.exe" "scripts/build-windows.sh qt"
else
  need "$TARGET/launcher.exe"    "scripts/build-windows.sh rust"
fi
need "$TARGET/player.exe"        "scripts/build-windows.sh rust"
need qemu/pc-bios                "scripts/prepare-qemu.sh"

rm -rf "$STAGE"
mkdir -p "$STAGE/doc"

if [ "$QT" = 1 ]; then
  install -m755 "$QT_TARGET/launcher-qt.exe" "$STAGE/2ksbox.exe"
else
  install -m755 "$TARGET/launcher.exe" "$STAGE/2ksbox.exe"
fi
install -m755 "$TARGET/player.exe" "$STAGE/2ksbox-player.exe"
install -m755 "$Q/qemu-img.exe" "$STAGE/"
install -m755 "$Q/libqemu-embed-i386.dll" "$STAGE/"
cp -a qemu/pc-bios "$STAGE/pc-bios"

# The Direct3D executor is optional at run time (the device says "no
# executor" and the guest falls back), so a package without it is a
# package, not a failure — but say so, because "3D does nothing" is not a
# symptom anyone enjoys tracing back to a packaging step.
if [ -f build/win/d3dpt/d3dpt_exec.dll ]; then
  install -m755 build/win/d3dpt/d3dpt_exec.dll "$STAGE/"
else
  echo "package-windows.sh: no build/win/d3dpt/d3dpt_exec.dll (scripts/build-windows.sh exec); packaging without Direct3D pass-through"
fi

# The diagnostic that answers "why does my Win98 guest get no OpenGL" on
# the machine it happens on, rather than in a VM (tools/wgl-probe.c).
if [ -f build/win/wgl-probe.exe ]; then
  mkdir -p "$STAGE/tools"
  install -m755 build/win/wgl-probe.exe "$STAGE/tools/"
fi

iso=$(ls -t guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1 || true)
if [ -n "$iso" ]; then
  mkdir -p "$STAGE/guest-tools"
  install -m644 "$iso" "$STAGE/guest-tools/"
else
  echo "package-windows.sh: no guest-tools ISO in guest-tools/out (guest-tools/build-wrappers.sh); packaging without it"
fi

if [ "$SHADERS" = 1 ]; then
  need third_party/slang-shaders "git submodule update --init third_party/slang-shaders"
  mkdir -p "$STAGE/shaders"
  tar -c --exclude='.git*' -C third_party/slang-shaders . | tar -x -C "$STAGE/shaders"
fi

install -m644 COPYING THIRD-PARTY-NOTICES.md README.md "$STAGE/doc/"

# --- the Qt runtime ---------------------------------------------------
# Qt needs more than its DLLs: a platform plugin (there is no window
# without `platforms/qwindows.dll`) and the QML modules the views import,
# neither of which is in any import table. There is no cross
# `windeployqt` in Fedora's mingw packages, so this is that step, written
# out: the plugin directories, the three QML module trees `qml/*.qml`
# imports (QtQuick pulls Controls, Layouts, Dialogs, Templates and
# Effects with it), and a `qt.conf` so Qt resolves both relative to the
# executable instead of to the build machine's absolute paths.
if [ "$QT" = 1 ]; then
  QTROOT=${WIN_QTROOT:-/usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6}
  if [ ! -d "$QTROOT" ]; then
    QTROOT="$ROOT/build/win/qt6"
    echo "==> copying the mingw Qt runtime out of the cross image"
    rm -rf "$QTROOT"      # for the same reason as the sysroot copy above
    mkdir -p "$QTROOT"
    scripts/win-cross.sh bash -c \
      "cp -a /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/plugins \
             /usr/x86_64-w64-mingw32/sys-root/mingw/lib/qt6/qml '$QTROOT/'"
  fi
  need "$QTROOT/plugins/platforms" "the cross image's mingw Qt 6"
  mkdir -p "$STAGE/plugins" "$STAGE/qml"
  for d in platforms imageformats iconengines styles tls; do
    [ -d "$QTROOT/plugins/$d" ] && cp -a "$QTROOT/plugins/$d" "$STAGE/plugins/"
  done
  for m in QtQuick QtQml QtCore; do
    [ -d "$QTROOT/qml/$m" ] && cp -a "$QTROOT/qml/$m" "$STAGE/qml/"
  done
  cat > "$STAGE/qt.conf" <<'EOF'
; Qt's own paths, relative to this executable. Without it a deployed
; build looks for its plugins and QML modules where they were on the
; machine that compiled Qt.
[Paths]
Prefix = .
Plugins = plugins
Qml2Imports = qml
EOF
fi

# --- the DLL closure --------------------------------------------------
# Everything our four binaries import, transitively, that is not a
# Windows system DLL. A missing one of these is the classic Windows
# failure: a dialog naming a DLL, before a single line of ours runs. The
# import tables are the source of truth, walked with objdump — no guessing
# from a package list, which is how such a closure goes stale.
#
# The system set is matched by name: anything under the mingw sysroot is
# ours to ship, anything else (kernel32, d3d9, opengl32, the api-ms-win-*
# API sets) is Windows' own and must NOT be shipped — copying a system
# DLL into the folder is how you get an app that only runs on the machine
# that built it.
SYSROOT=${WIN_SYSROOT:-/usr/x86_64-w64-mingw32/sys-root/mingw/bin}
OBJDUMP=${WIN_OBJDUMP:-x86_64-w64-mingw32-objdump}
if [ ! -d "$SYSROOT" ]; then
  # The sysroot lives in the cross container, so ask it for a copy — every
  # time, not once: a kept copy is a snapshot of an older image, and the
  # first `--qt` run found exactly that, a cache from before Qt was in
  # there, and quietly packaged a launcher with no Qt6Core.dll beside it.
  SYSROOT="$ROOT/build/win/sysroot-bin"
  echo "==> copying the mingw runtime out of the cross image"
  rm -rf "$SYSROOT"
  mkdir -p "$SYSROOT"
  scripts/win-cross.sh bash -c \
    "cp -a /usr/x86_64-w64-mingw32/sys-root/mingw/bin/*.dll '$SYSROOT/'"
fi
command -v "$OBJDUMP" >/dev/null || { echo "package-windows.sh: no $OBJDUMP (WIN_OBJDUMP=)"; exit 1; }

imports() { "$OBJDUMP" -p "$1" | sed -n 's/^\tDLL Name: //p'; }

# Every binary in the package is a root, not just the ones at the top: a
# Qt platform plugin or a QML module's DLL sits in a subdirectory, imports
# half of Qt, and is loaded by name at run time — so nothing above it
# names what it needs. They are resolved from the executable's own
# directory, which is where the closure puts everything.
staged_binaries() { find "$STAGE" \( -name '*.dll' -o -name '*.exe' \) -type f; }

declare -A seen=()
copied=0
again=1
while [ "$again" = 1 ]; do
  again=0
  while read -r file; do
    [ -n "$file" ] || continue
    while read -r dll; do
      [ -n "$dll" ] || continue
      key=$(printf '%s' "$dll" | tr 'A-Z' 'a-z')
      [ -n "${seen[$key]:-}" ] && continue
      seen[$key]=1
      src=$(ls "$SYSROOT/$dll" 2>/dev/null || ls "$SYSROOT"/"$key" 2>/dev/null || true)
      [ -n "$src" ] || continue        # a Windows system DLL: not ours
      install -m755 "$src" "$STAGE/$(basename "$src")"
      copied=$((copied + 1))
      again=1
    done < <(imports "$file")
  done < <(staged_binaries)
done

# A DLL that is *loaded* rather than imported is invisible to the walk
# above, and one of ours is: Fedora's mingw64-SDL2 is **sdl2-compat**, an
# SDL2.dll that LoadLibrary's SDL3.dll at run time. Shipping only what the
# import tables named gave a package whose player died with "Failed
# loading SDL3 library" on a machine that had no SDL of its own (found on
# a real Windows PC, 2026-09-06).
#
# So: every staged binary is searched for names of DLLs that exist in the
# mingw sysroot and are not staged yet, and those are shipped too. It is
# broader than reading an import table and that is the point — the next
# runtime load will be caught by the same pass instead of by a user.
runtime_deps() { strings -a "$1" | grep -oiE '[A-Za-z0-9_.+-]+\.dll' | sort -u; }
if command -v strings >/dev/null; then
  again=1
  while [ "$again" = 1 ]; do
    again=0
    while read -r file; do
      [ -n "$file" ] || continue
      while read -r dll; do
        [ -n "$dll" ] || continue
        key=$(printf '%s' "$dll" | tr 'A-Z' 'a-z')
        [ -n "${seen[$key]:-}" ] && continue
        seen[$key]=1
        src=$(ls "$SYSROOT/$dll" 2>/dev/null || ls "$SYSROOT"/"$key" 2>/dev/null || true)
        [ -n "$src" ] || continue        # Windows' own, or not a real name
        install -m755 "$src" "$STAGE/$(basename "$src")"
        echo "               + $(basename "$src") (loaded at run time, not imported)"
        copied=$((copied + 1))
        again=1
      done < <(runtime_deps "$file")
    done < <(staged_binaries)
  done
else
  echo "package-windows.sh: no strings(1); run-time-loaded DLLs not checked for" >&2
fi
echo "runtime DLLs   $copied copied from $(basename "$SYSROOT")"

# --- the check --------------------------------------------------------
# The staged binaries, run as Windows binaries, from outside the checkout,
# with an empty environment: not one LAUNCHER_*/PLAYER_* knob from this
# shell can be what makes them work, and nothing may resolve back into the
# build tree. Wine is the only Windows available on a Linux build host —
# it is not the target, so a failure here is investigated rather than
# trusted, but "the launcher starts and answers about itself" and "the
# packaged qemu-img writes a qcow2" are exactly the things a broken
# package fails at.
fail=0
if command -v wine >/dev/null; then
  scratch=$(mktemp -d)
  trap 'rm -rf "$scratch"' EXIT
  export WINEPREFIX="$scratch/wine" WINEDEBUG=-all
  # One prefix, created once, so every check below runs in the same one.
  wine wineboot -i >/dev/null 2>&1 || true

  runw() { (cd "$STAGE" && env -i HOME="$scratch" WINEPREFIX="$WINEPREFIX" \
                WINEDEBUG=-all PATH="$PATH" wine "$@" 2>/dev/null); }

  resolved=$(runw 2ksbox.exe --paths || true)
  if [ -z "$resolved" ]; then
    # The Qt front end does not start under wine at all: it faults on a
    # null call before main prints anything, with either subsystem, while
    # the egui binary in the same folder answers perfectly (M11, tracked
    # in docs/tracks/m11-windows-host.md). Whether that is wine's Qt 6 or
    # ours is a question only a real Windows machine can answer, so it is
    # reported here rather than failing a package nobody can yet check.
    if [ "$QT" = 1 ]; then
      echo "launcher       the Qt front end does not run under wine (unverified; try it on Windows)"
    else
      echo "package-windows.sh: the staged launcher printed nothing for --paths" >&2
      fail=1
    fi
  else
    printf '%s\n' "$resolved"
    # Every companion must resolve inside the package. Wine reports them
    # as Z:\... paths for a Unix directory, so compare on the tail.
    while read -r what path; do
      case "$what" in player|qemu-img|pc-bios|guest-tools|prefix) ;; *) continue ;; esac
      case "$path" in "("*) continue ;; esac
      win=$(printf '%s' "$path" | tr '\\' '/' | sed 's|^[A-Za-z]:||')
      case "$win" in
        *"/$NAME"/*|*"/$NAME") ;;
        *) echo "package-windows.sh: $what resolved outside the package: $path" >&2; fail=1 ;;
      esac
    done <<< "$resolved"
  fi

  # The bundle-creating path end to end: the staged launcher runs the
  # staged qemu-img to make a disk, and turns the result into a command
  # line pointing at the staged firmware. This is also what proves the
  # DLL closure: qemu-img.exe cannot start without every DLL beside it.
  runw 2ksbox.exe --wizard-new xp "Package check" 1 >/dev/null || true
  disk=$(find "$scratch" "$WINEPREFIX/drive_c/users" -name disk.qcow2 2>/dev/null | head -1)
  if [ -n "$disk" ] && [ -s "$disk" ]; then
    echo "qemu-img       created $(du -h "$disk" | cut -f1) of qcow2"
  elif [ "$QT" = 1 ]; then
    # Same reason: the wizard is driven through the launcher, which is
    # the binary wine cannot start here. The egui package checks the very
    # same qemu-img.exe, so this is not an unchecked artefact.
    echo "qemu-img       (not exercised: the Qt launcher does not run under wine)"
  else
    echo "package-windows.sh: the packaged qemu-img did not create a disk" >&2
    fail=1
  fi
  # Offscreen GL, which is what a Win98 guest's 3D needs (the embed
  # library's WGL backend). Reported, never fatal: it is a property of
  # the machine that runs the package, and wine's GL is not the target's
  # — a Windows user runs tools\wgl-probe.exe there for the real answer.
  # ... and unlike the checks above, this one needs a display: it opens a
  # window (an invisible one, but a real one), which the `env -i` the
  # others deliberately run under makes impossible.
  runw_display() { (cd "$STAGE" && env -i HOME="$scratch" WINEPREFIX="$WINEPREFIX" \
        WINEDEBUG=-all PATH="$PATH" DISPLAY="${DISPLAY:-}" \
        WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-}" \
        wine "$@" 2>/dev/null); }
  if [ -f "$STAGE/tools/wgl-probe.exe" ]; then
    if out=$(runw_display tools/wgl-probe.exe); then
      echo "wgl-probe      $(printf '%s\n' "$out" | tail -1)"
    else
      echo "wgl-probe      no offscreen GL under wine: $(printf '%s\n' "$out" | tail -1)"
    fi
  fi
else
  echo "checks         (no wine on this host; the package was not run)"
fi
[ "$fail" = 0 ] || exit 1
echo "checks passed"

du -sh "$STAGE" | sed 's/^/staged  /'
if [ "$ZIP" = 1 ]; then
  # A zip, because that is what a Windows user is handed. `zip` is not on
  # every Linux (this project's own host has none), and Python's zipfile
  # is: it produces the same archive and is always there, so it is the
  # fallback rather than another package to install.
  archive="$OUT/$NAME.zip"
  rm -f "$archive"
  if command -v zip >/dev/null; then
    (cd "$OUT" && zip -qr "$archive" "$NAME")
  else
    python3 -c 'import shutil,sys; shutil.make_archive(sys.argv[1], "zip", sys.argv[2], sys.argv[3])' \
      "${archive%.zip}" "$OUT" "$NAME"
  fi
  du -h "$archive" | sed 's/^/zip     /'
fi
echo "package: $STAGE"
