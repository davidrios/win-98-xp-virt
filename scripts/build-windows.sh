#!/usr/bin/env bash
# Build the Windows artefacts, in dependency order, from a Linux host --
# the counterpart of scripts/build.sh, which builds for the host it runs
# on. Everything happens inside the cross container
# (scripts/win-cross.sh, packaging/windows/Dockerfile) except the guest
# tools and the packaging step, which are host-side by nature.
#
#   scripts/build-windows.sh                everything this host can build
#   scripts/build-windows.sh qemu rust      only those stages
#   scripts/build-windows.sh --package      ... and then roll the zip
#
# Stages, in the order they must run:
#
#   qemu    configure-qemu.sh --windows -> ninja: qemu-system-i386.exe,
#           qemu-img.exe, qemu-io.exe, libqemu-embed-i386.dll, into
#           build/win/qemu (with libdisc built for windows-gnu first)
#   rust    cargo build --release --target x86_64-pc-windows-gnu: the
#           player, the launcher, discx. After `qemu`, because the player
#           links the embed DLL out of build/win/qemu.
#   qt      cargo build in launcher-qt/ (its own workspace): the Qt 6 /
#           QML front end, the second of doc 07's two maintained ones.
#           Cross-compiled like everything else — the image carries the
#           mingw Qt to link against and the native Qt of the same
#           version for moc/rcc/qmltyperegistrar.
#   exec    build-d3dpt-exec.sh --windows: d3dpt_exec.dll, the Direct3D
#           executor (doc 14). No DXVK build: on Windows the host has a
#           Direct3D 9, and a DXVK d3d9.dll dropped next to the player
#           overrides it.
#   guest   guest-tools/build-wrappers.sh: the guest-tools ISO. Host-side
#           and host-independent -- the ISO is 32-bit guest code, the same
#           file the Linux package ships -- so it is only built here when
#           there is not one already.
#
# docs/build-windows.md is the prose; docs/tracks/m11-windows-host.md is
# the track. Nothing here writes to build/qemu or target/release: a
# checkout holds a Linux build and a Windows build side by side.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

JOBS=(); PACKAGE=""; STAGES=(); EXPLICIT=""
while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS=(-j "$2"); shift 2 ;;
    -j*) JOBS=(-j "${1#-j}"); shift ;;
    -p|--package) PACKAGE=1; shift ;;
    -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    qemu|rust|qt|exec|guest) STAGES+=("$1"); shift ;;
    *) echo "build-windows.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
  esac
done
if [ ${#STAGES[@]} -eq 0 ]; then STAGES=(qemu rust qt exec guest); else EXPLICIT=1; fi

BUILT=(); SKIPPED=(); T0=$SECONDS
want() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
skip() { # stage, reason
  if [ -n "$EXPLICIT" ]; then echo "build-windows.sh: cannot build '$1': $2" >&2; exit 1; fi
  echo "    SKIP $1 - $2"; SKIPPED+=("$1 ($2)"); return 1
}
inw() { scripts/win-cross.sh "$@"; }

if [ ! -f qemu/VERSION ] || [ ! -f third_party/qemu-3dfx/00-qemu92x-mesa-glide.patch ]; then
  say "git submodule update --init (qemu, qemu-3dfx)"
  git submodule update --init --depth 1 qemu third_party/qemu-3dfx
fi

# The patch queue is applied to the one qemu/ tree both builds compile
# from, so it is prepared here exactly as scripts/build.sh does it -- and
# never twice in a row for nothing, which is what the stamp is for there.
# Here it is unconditional but cheap: a Windows build is not the inner
# loop, and a tree left half-prepared by an interrupted native build is
# the failure that costs an hour.
if want qemu; then
  say "qemu: prepare (overlay + patch queue)"
  scripts/prepare-qemu.sh

  if [ ! -f build/win/qemu/build.ninja ]; then
    say "qemu: configure (mingw-w64 cross)"
    inw scripts/configure-qemu.sh --windows
  else
    echo "    build/win/qemu is configured - skipping configure"
    # prepare re-applied the queue, so meson may need to regenerate; ninja
    # works that out itself from the mtimes it just saw change.
    :
  fi
  say "qemu: ninja"
  inw ninja -C build/win/qemu ${JOBS[@]+"${JOBS[@]}"} \
    qemu-system-i386.exe qemu-img.exe qemu-io.exe libqemu-embed-i386.dll
  BUILT+=(qemu)
fi

if want rust; then
  say "rust: cargo build --release --target x86_64-pc-windows-gnu"
  inw cargo build --release --target x86_64-pc-windows-gnu --workspace ${JOBS[@]+"${JOBS[@]}"}
  BUILT+=(rust)
fi

# launcher-qt is its own cargo workspace (so a plain `cargo build` never
# needs Qt), which is why this is a separate stage with its own directory
# rather than another member of the one above.
if want qt; then
  if ! scripts/win-cross.sh test -x /usr/bin/x86_64-w64-mingw32-qmake-qt6; then
    skip qt "the cross image has no mingw Qt 6 (rebuild it: scripts/win-cross.sh --build)" || true
  else
    say "qt: cargo build --release --target x86_64-pc-windows-gnu (launcher-qt)"
    inw sh -c 'cd launcher-qt && exec cargo build --release --target x86_64-pc-windows-gnu'
    BUILT+=(qt)
  fi
fi

if want exec; then
  say "exec: d3dpt_exec.dll (the Direct3D decoder + executor)"
  inw scripts/build-d3dpt-exec.sh --windows
  # The WGL probe rides along: it is the same 3D stage, it is one
  # compile, and it is the first thing to run on a Windows machine whose
  # Win98 guest gets no OpenGL (tools/wgl-probe.c).
  say "exec: wgl-probe.exe (the embed backend's WGL sequence, without QEMU)"
  inw x86_64-w64-mingw32-gcc -O1 -o build/win/wgl-probe.exe tools/wgl-probe.c \
    -lopengl32 -lgdi32 -luser32
  BUILT+=(exec)
fi

# The ISO is guest code and identical whatever host built it, so this
# stage exists to notice that there is none rather than to rebuild one.
if want guest; then
  if ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1; then
    say "guest"
    echo "    guest-tools ISO present - skipping (guest-tools/build-wrappers.sh rebuilds it)"
  elif ! command -v i686-w64-mingw32-gcc >/dev/null; then
    skip guest "needs mingw-w64 (i686-w64-mingw32-gcc)" || true
  elif ! command -v xorriso >/dev/null && ! command -v genisoimage >/dev/null; then
    skip guest "needs xorriso (or genisoimage) for the ISO" || true
  else
    say "guest: guest-tools ISO"
    guest-tools/build-wrappers.sh
    BUILT+=(guest)
  fi
fi

say "summary after $((SECONDS - T0)) s"
[ ${#BUILT[@]} -gt 0 ] && printf '    built: %s\n' "${BUILT[*]}"
if [ ${#SKIPPED[@]} -gt 0 ]; then printf '    skipped:\n'; printf '      %s\n' "${SKIPPED[@]}"; fi

if [ -n "$PACKAGE" ]; then
  say "scripts/package-windows.sh"
  exec scripts/package-windows.sh
fi
echo
echo "    next: scripts/package-windows.sh   (the zip, checked under wine)"
