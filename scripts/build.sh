#!/usr/bin/env bash
# Build everything, in dependency order, so nobody has to remember the
# order or the per-platform flags:
#
#   scripts/build.sh                 everything this host can build
#   scripts/build.sh qemu rust       only those stages
#   scripts/build.sh --test          everything, then scripts/test.sh host
#
# Stages, in the order they must run:
#
#   qemu    prepare-qemu.sh (overlay + patch queue) -> configure-qemu.sh
#           -> ninja: qemu-system-i386, qemu-img, qemu-io,
#           libqemu-embed-i386.{so,dylib}
#   rust    cargo build --release: player, launcher, libdisc/discx,
#           qemu-embed, shader-chain. After `qemu`, because the player
#           links libqemu-embed out of build/qemu.
#   dxvk    prepare-dxvk.sh -> configure-dxvk.sh -> ninja
#   exec    build-d3dpt-exec.sh: libd3dpt_exec, the D3D executor. After
#           `dxvk`, whose headers it compiles against.
#   guest   guest-tools/build-wrappers.sh: the guest-tools ISO (which
#           calls build-driver.sh for the XP display driver too)
#
# A stage whose tools are missing is SKIPped with the reason, unless it
# was named on the command line -- then it is an error. The summary at
# the end is the honest account of what this host actually has.
#
# This exists because a partial rebuild is the project's most expensive
# mistake: a stale libd3dpt_exec or guest-tools ISO after a
# D3DPT_PROTO_VERSION bump reads as "the guest will not boot", not as
# "you forgot a command" (docs/00-status.md's cheat sheet).
#
# Running it when everything is up to date costs a couple of seconds
# thanks to the stamps below, and needs no network -- which a bare
# `guest` stage does, since it fetches wine9x.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

case "$(uname -s)" in Darwin) SO=dylib;; *) SO=so;; esac

JOBS=()
RUN_TEST=""
FORCE=""
STAGES=()

usage() {
  sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
  cat <<EOF

Options:
  -j N            parallel jobs for ninja and cargo (default: auto)
  -f, --force     re-run every prepare and configure step, ignoring the
                  stamps -- the escape hatch when a tree was edited by hand
  -t, --test      run scripts/test.sh host when the build succeeds
  -h, --help      this text
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS=(-j "$2"); shift 2 ;;
    -j*) JOBS=(-j "${1#-j}"); shift ;;
    -f|--force) FORCE=1; shift ;;
    -t|--test) RUN_TEST=1; shift ;;
    -h|--help) usage; exit 0 ;;
    qemu|rust|dxvk|exec|guest) STAGES+=("$1"); shift ;;
    *) echo "build.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
  esac
done

EXPLICIT=""
if [ ${#STAGES[@]} -eq 0 ]; then
  STAGES=(qemu rust dxvk exec guest)
else
  EXPLICIT=1
fi

BUILT=(); SKIPPED=(); T0=$SECONDS

want() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# A stage this host cannot run: an error when it was asked for by name, a
# recorded skip when we were just building everything.
skip() { # stage, reason
  if [ -n "$EXPLICIT" ]; then echo "build.sh: cannot build '$1': $2" >&2; exit 1; fi
  echo "    SKIP $1 - $2"
  SKIPPED+=("$1 ($2)")
  return 1
}

# --- stamps -----------------------------------------------------------
# Every "prepare" step here restores its tree and re-applies a patch
# queue, which hands the build system a few thousand fresh mtimes and a
# from-scratch rebuild EVERY run -- four minutes for a tree with nothing
# to do. So each is guarded by a hash of what actually feeds it: the patch
# queue, the overlaid sources, the submodule commits, the prepare script
# itself. Skipping a prepare leaves the tree exactly as the last one left
# it, which is the state the "never git checkout inside qemu/ by hand"
# rule already protects; -f is the escape hatch for when it was broken.
if have sha256sum; then SHA=(sha256sum); else SHA=(shasum -a 256); fi

stamp_value() { # $STAMP_GITS holds git repos to pin; arguments are paths
  {
    local g
    for g in ${STAMP_GITS:-}; do git -C "$g" rev-parse HEAD 2>/dev/null || echo none; done
    find "$@" -type f 2>/dev/null | LC_ALL=C sort | tr '\n' '\0' | xargs -0 cat 2>/dev/null
  } | "${SHA[@]}" | cut -d' ' -f1
}

# usage: if stamp_stale <name> <paths...>; then <prepare>; stamp_save; fi
stamp_stale() {
  STAMP_FILE="build/.stamp-$1"; shift
  mkdir -p build
  STAMP_VALUE="$(stamp_value "$@")"
  [ -n "$FORCE" ] && return 0
  [ "$(cat "$STAMP_FILE" 2>/dev/null || true)" != "$STAMP_VALUE" ]
}
stamp_save() { printf '%s\n' "$STAMP_VALUE" > "$STAMP_FILE"; }

# macOS: the deployment target must be identical for configure-qemu.sh and
# for cargo, or ld warns "built for newer macOS version" on every C++ dep
# and on libqemu. Set it once so both stages inherit the same value
# (CLAUDE.md's macOS note).
if [ "$(uname -s)" = Darwin ] && [ -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]; then
  MACOSX_DEPLOYMENT_TARGET="$(sw_vers -productVersion | cut -d. -f1,2)"
  export MACOSX_DEPLOYMENT_TARGET
  echo "==> MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET (configure and cargo alike)"
fi

# --- submodules -------------------------------------------------------
# Cloning without --recurse-submodules is the most common first failure.
if [ ! -f qemu/VERSION ] || [ ! -f third_party/qemu-3dfx/00-qemu92x-mesa-glide.patch ]; then
  say "git submodule update --init (qemu, qemu-3dfx)"
  git submodule update --init --depth 1 qemu third_party/qemu-3dfx
fi

# --- qemu -------------------------------------------------------------
if want qemu; then
  if ! have ninja; then skip qemu "no ninja" || true
  else
    say "qemu"
    if STAMP_GITS="qemu third_party/qemu-3dfx" \
       stamp_stale qemu-prepare patches/qemu embed d3dpt/hw d3dpt/d3dpt_proto.h \
         d3dpt/d3dpt_fb.h d3dpt/exec/d3dpt_exec.h libdisc/qemu libdisc/libdisc.h \
         scripts/prepare-qemu.sh third_party/qemu-3dfx/00-qemu92x-mesa-glide.patch; then
      scripts/prepare-qemu.sh
      stamp_save
    else
      echo "    patch queue, overlays and submodules unchanged - skipping prepare"
    fi

    # configure resets meson options and is slow, so only when needed.
    # prepare-qemu.sh deliberately preserves meson.build mtimes when the
    # content is unchanged, which is what makes this comparison meaningful.
    needs_configure=""
    [ -n "$FORCE" ] && needs_configure=1
    [ -f build/qemu/build.ninja ] || needs_configure=1
    for f in qemu/meson.build qemu/hw/3dfx/meson.build qemu/hw/mesa/meson.build; do
      if [ -f "$f" ] && [ -f build/qemu/build.ninja ] && [ "$f" -nt build/qemu/build.ninja ]; then
        needs_configure=1
      fi
    done
    if [ -n "$needs_configure" ]; then
      if [ -z "${QEMU_PYTHON:-}" ] && ! have uv; then
        skip qemu "configure needs uv (or QEMU_PYTHON=<python 3.8-3.13>)" || true
      else
        say "qemu: configure"
        scripts/configure-qemu.sh
      fi
    else
      echo "    build/qemu is configured and no meson file moved - skipping configure"
    fi

    if [ -f build/qemu/build.ninja ]; then
      say "qemu: ninja"
      ninja -C build/qemu ${JOBS[@]+"${JOBS[@]}"} \
        qemu-system-i386 qemu-img qemu-io "libqemu-embed-i386.$SO"
      BUILT+=(qemu)
    fi
  fi
fi

# --- rust -------------------------------------------------------------
# After qemu: the player links libqemu-embed with an rpath into
# build/qemu, and qemu-embed/build.rs warns when qemu/embed/ is a stale
# copy of embed/ (which is why prepare-qemu.sh ran first).
if want rust; then
  if ! have cargo; then skip rust "no cargo" || true
  else
    say "rust: cargo build --release (workspace)"
    cargo build --release --workspace ${JOBS[@]+"${JOBS[@]}"}
    BUILT+=(rust)
  fi
fi

# --- dxvk -------------------------------------------------------------
if want dxvk; then
  if ! have meson || ! have ninja; then skip dxvk "needs meson and ninja" || true
  elif ! have glslangValidator && ! have glslang; then
    skip dxvk "needs glslang (vulkan-headers, vulkan-loader, glslang, sdl2)" || true
  else
    say "dxvk"
    if STAMP_GITS="third_party/dxvk" \
       stamp_stale dxvk-prepare patches/dxvk scripts/prepare-dxvk.sh; then
      scripts/prepare-dxvk.sh
      stamp_save
    else
      echo "    patch queue and submodule unchanged - skipping prepare"
    fi
    if [ -n "$FORCE" ] || [ ! -f build/dxvk/build.ninja ]; then
      say "dxvk: configure"
      scripts/configure-dxvk.sh
    fi
    say "dxvk: ninja"
    ninja -C build/dxvk ${JOBS[@]+"${JOBS[@]}"}
    BUILT+=(dxvk)
  fi
fi

# --- exec -------------------------------------------------------------
# Compiles against third_party/dxvk's native headers; the DXVK library
# itself is dlopened at runtime, so this needs the tree prepared, not built.
if want exec; then
  if [ ! -f third_party/dxvk/include/native/windows/windows_base.h ]; then
    skip exec "third_party/dxvk not prepared (run the dxvk stage first)" || true
  else
    say "exec: libd3dpt_exec (the D3D decoder + DXVK executor)"
    scripts/build-d3dpt-exec.sh
    BUILT+=(exec)
  fi
fi

# --- guest ------------------------------------------------------------
# The guest-tools ISO carries the D3DPT guest DLLs, so a protocol bump
# makes it stale exactly as it does the executor. build-wrappers.sh also
# calls build-driver.sh, so the XP display driver rides along -- and it
# fetches wine9x, which is why the stamp is what keeps this offline-able.
GUEST_STALE=""
if want guest; then
  # the stamp is computed before the tool check, so a host that cannot
  # build the ISO can still say whether the one it has is out of date
  if STAMP_GITS="third_party/qemu-3dfx" \
     stamp_stale guest-tools guest-tools/src patches/wine9x d3dpt/d3dpt_proto.h \
       d3dpt/d3dpt_fb.h cdshelf/cdshelf_proto.h \
       guest-tools/build-wrappers.sh guest-tools/build-driver.sh; then
    GUEST_STALE=1
  fi
  # a stamp is no good without the artifacts it claims are current
  [ -e guest-tools/out/d3dpt-driver.iso ] || GUEST_STALE=1
  ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1 || GUEST_STALE=1

  if ! have i686-w64-mingw32-gcc; then
    skip guest "needs mingw-w64 (i686-w64-mingw32-gcc)" || true
  elif ! have xorriso && ! have genisoimage && ! have mkisofs; then
    skip guest "needs xorriso (or genisoimage/mkisofs) for the ISO" || true
  elif [ -n "$GUEST_STALE" ]; then
    say "guest: guest-tools ISO + XP display driver"
    guest-tools/build-wrappers.sh
    stamp_save
    GUEST_STALE=""
    BUILT+=(guest)
  else
    say "guest"
    echo "    guest sources, protocol headers and qemu-3dfx unchanged - skipping"
  fi
fi

# --- summary ----------------------------------------------------------
say "summary after $((SECONDS - T0)) s"
if [ ${#BUILT[@]} -gt 0 ]; then printf '    built: %s\n' "${BUILT[*]}"; fi
if [ ${#SKIPPED[@]} -gt 0 ]; then
  printf '    skipped:\n'
  printf '      %s\n' "${SKIPPED[@]}"
fi

# The failure this script exists to prevent: an artifact left behind by a
# stage this host could not run. A stage that ran refreshed its own output;
# a stage that was skipped for a missing tool is the case nobody looks at,
# and a guest-tools ISO older than the protocol it speaks does not announce
# itself -- it reads as an XP guest that will not attach.
stale=()
if [ -n "$GUEST_STALE" ]; then
  for iso in guest-tools/out/*.iso; do
    [ -e "$iso" ] || continue
    stale+=("$iso (guest-tools/build-wrappers.sh)")
  done
fi
for s in ${SKIPPED[@]+"${SKIPPED[@]}"}; do
  case "$s" in
    exec*) [ -f "build/d3dpt/libd3dpt_exec.$SO" ] \
             && stale+=("build/d3dpt/libd3dpt_exec.$SO (scripts/build-d3dpt-exec.sh)") ;;
  esac
done
if [ ${#stale[@]} -gt 0 ]; then
  printf '\n    WARNING: these are older than the sources they are built from, and\n'
  printf '    this host cannot rebuild them. A stale guest-tools ISO after a\n'
  printf '    D3DPT_PROTO_VERSION bump reads as a guest that will not attach:\n'
  printf '      %s\n' "${stale[@]}"
fi

if [ -n "$RUN_TEST" ]; then
  say "scripts/test.sh host"
  exec scripts/test.sh host
fi

echo
echo "    next: scripts/test.sh        (host stage, ~30 s)"
echo "          scripts/test.sh all    (adds the XP and DOS guests, ~2 min)"
