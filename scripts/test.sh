#!/usr/bin/env bash
# The regression suite: integration and end-to-end only (CLAUDE.md policy).
#
#   scripts/test.sh            host stage: every check that needs no guest
#   scripts/test.sh guest      XP on the paravirtual Direct3D device, headless
#   scripts/test.sh all        both
#
# Builds nothing big: it expects build/qemu (qemu-system-i386 +
# libqemu-embed), build/dxvk and build/d3dpt/libd3dpt_exec.* to exist (the
# cheat sheet in docs/00-status.md), and only compiles the small tools in
# tools/. Outputs land in build/test/. A check that cannot run here (no
# x86 host, no display, no image) is reported as SKIP, not as a failure.
#
# Host stage
#   x87-fast       tools/x87-fast-test.c: patch 05's fast path vs the real x87
#   libdisc        discx selftest (doc 17 §6.1): synthetic cue/bin, CCD and ISO
#                  images, the CD model's reads, EDC/ECC, Q synthesis and the
#                  MMC responders checked through libdisc's C API
#   dirdisc        a host directory served as a disc (isodir, M5g): discx generates
#                  the ISO 9660 + Joliet volume over a fixture tree, exports it and
#                  xorriso (or bsdtar) reads the folder back out identical; then
#                  qemu-img reads the same bytes through the block layer and
#                  SeaBIOS probing the drive proves the ATAPI path finds the disc
#                  model (a plain .iso on the raw driver is the control)
#   cdimage        the cdimage block driver (patch 50) through QEMU's block layer:
#                  qemu-img probes the cue and the ccd to "cdimage" with the
#                  lead-out × 2048 as the size, the data track dd'd out equals the
#                  ISO, a plain .iso still probes to raw
#   package        scripts/package-linux.sh: the Linux install layout staged from
#                  this build, and the staged launcher asked with a scrubbed
#                  environment whether player/qemu-img/firmware/guest-tools all
#                  resolve inside the package (doc 07's install layout)
#   optimizations  the wizard's fast-path switches (patches/qemu/README.md) from a
#                  checkbox to a real QEMU: a default machine's line unchanged,
#                  each switch on the option QEMU looks it up on, our QEMU
#                  accepting the line the launcher writes
#   capi           launcher-capi/examples/smoke.c: a third front end, in C, over
#                  the same models the egui and Qt builds use — the wizard's
#                  DOS defaults, the disc shelf, snapshots and the profile
#                  editor, driven through include/launcher_core.h (doc 07)
#   preview-anim   the launcher's shader preview keeps drawing (doc 07): a preset
#                  that stands still says so and renders the same picture at any
#                  frame number, one that does not (an interlaced CRT) says so
#                  and renders two different pictures at two frame numbers
#   embed-3d       tools/embed-3d-test.c: the window-less Mesa backend (Linux)
#   glide-host     tools/glide-host-test.cpp: Glide pass-through without a guest
#                  (Linux) — the real host wrapper loaded by hw/3dfx, opened
#                  through glidewnd.c's handshake, a triangle checked in the
#                  frame the frontend receives, orientation included
#   d3dpt-exec     tools/d3dpt-exec-test.cpp: guest encoder → decoder → DXVK,
#                  frames delivered, hostile batch refused
#   d3dpt-dp2      tools/d3dpt-dp2-test.cpp: the display driver's records (doc 15
#                  M7c): VRAM surfaces, a context, the D3D7TEST scene as DP2
#                  tokens, readback pixels checked, hostile records refused
#   d3dgame9-nat   the reference scene natively on DXVK: frame 300 within
#                  D3D_GOLDEN_BUDGET pixels (tolerance 8, HUD masked) of the
#                  rig golden; this frame is the oracle for the guest stage
#   d3dfeat9-nat   the feature test natively: frame + log lines kept as the
#                  oracle for the guest stage
#
# Guest stage
#   sse-guest      tools/sse-guest-test.py: the SSE battery, sse-fast on/off identical (doc 16)
#   atapi-guest    tools/atapi-guest-test.py: a DOS program drives the ATAPI drive
#                  on the selftest's flipped-sector cue by PIO (patch 51); every
#                  reply identical to discx's at byte-count limits 512 and 65534,
#                  then the disc shelf (patch 52) and a second boot running the
#                  real CDSHELF.COM against it
#   x87-guest      tools/x87-guest-test.py: a DOS x87 battery under TCG,
#                  identical with the fast path on and off (needs nasm,
#                  mtools and the FreeDOS floppy the tool fetches on first use)
#   rep-guest      tools/rep-guest-test.py: a DOS rep movs/stos battery (widths,
#                  address sizes, DF, page crossings, overlaps), rep-fast on/off
#                  identical and equal to a model of the instruction (patch 17)
#   smc-guest      tools/smc-guest-test.py: self-modifying code (patched immediates,
#                  same-value rewrites, opcode flips, a crossing store), smc-same-value
#                  on/off both architecturally right (patch 18)
#   guest-cdimage  tools/xp-cdimage-test.sh: XP boots with the guest-tools ISO
#                  converted to a cue (+ a 1 kHz tone track) as its CD-ROM and
#                  copies the whole disc through cdrom.sys; every file must
#                  match the ISO's; with mingw, CDTEST.EXE then plays the tone
#                  through MCI and the drive's audiodev (a wav) must carry it
#   XP (Linux; KVM when /dev/kvm is usable, TCG otherwise):
#   boots WINXP_IMG read-only (snapshot=on) with the newest guest-tools ISO
#   and a fresh FAT32 scratch disk carrying RUN.BAT, drives the Run dialog
#   over QMP, waits for the three programs to detach from the device, shuts
#   XP down (DDVMTEST first: the DirectDraw shim's video-memory answer), and
#   diffs: D3DGAME9 and D3DGAME8 pixel-identical to the native
#   D3DGAME9 frame outside the wall-time HUD (and within budget of the rig
#   golden), D3DFEAT9 byte-identical to the native frame with the same
#   query / getter lines.
#
# Environment: WINXP_IMG (~/vms/winxp.qcow2), GUEST_ISO (newest
# guest-tools/out/guest-tools-3dfx-*.iso), TEST_ACCEL (kvm|tcg),
# D3D_GOLDEN_BUDGET (1200), TEST_BOOT_TIMEOUT (300 s), TEST_KEEP=1 keeps
# the VM running on failure for a look (QMP socket printed).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/build/test"; mkdir -p "$OUT"
STAGE="${1:-host}"
OS="$(uname -s)"; ARCH="$(uname -m)"
DX="$ROOT/third_party/dxvk/include/native"
GOLDEN="$ROOT/reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp"
HUD_MASK="0,368,270,112"
BUDGET="${D3D_GOLDEN_BUDGET:-1200}"
case "$OS" in Darwin) SO=dylib;; *) SO=so;; esac
export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.$SO}"
export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.$SO$([ "$SO" = so ] && echo .0)}"
if [ "$OS" = Darwin ]; then
  # DXVK dlopens the Vulkan loader by leaf name and SDL2 needs it too; a
  # DYLD_* variable handed to this script is stripped by SIP at the
  # `#!/usr/bin/env` exec, so set the documented macOS run environment
  # here (docs/build-macos.md, patches/dxvk/README.md): Homebrew's loader,
  # and the LunarG SDK's KosmicKrisp ICD unless the caller chose one.
  export DYLD_LIBRARY_PATH="/opt/homebrew/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
  export SDL_VULKAN_LIBRARY="${SDL_VULKAN_LIBRARY:-/opt/homebrew/lib/libvulkan.dylib}"
  if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    for f in "$HOME"/VulkanSDK/*/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json; do
      [ -f "$f" ] && export VK_ICD_FILENAMES="$f"
    done
  fi
fi

PASS=(); FAIL=(); SKIP=()
log() { printf '\n==> %s\n' "$*"; }
skip() { SKIP+=("$1: $2"); printf '  SKIP %s (%s)\n' "$1" "$2"; }
run_check() { # name, log file, command...
  local name="$1" lf="$OUT/$2"; shift 2
  "$@" >"$lf" 2>&1; local rc=$?
  if [ $rc = 0 ]; then PASS+=("$name"); printf '  PASS %s\n' "$name"; return 0; fi
  if [ $rc = 77 ]; then skip "$name" "$(tail -1 "$lf")"; return 0; fi
  FAIL+=("$name"); printf '  FAIL %s (exit %d) — %s\n' "$name" $rc "$lf"; tail -5 "$lf" | sed 's/^/       /'; return 1
}
dirdisc_check() { # a host directory served as a disc, read back by someone else's ISO 9660 reader
  # discx's own dirdisc case (the libdisc check) proves the model reads
  # the tree back; this one proves the *volume* is one, by handing it to
  # a reader that is not ours and diffing the result against the folder.
  local src="$OUT/dirsrc" ext="$OUT/dirsrc-out" iso="$OUT/dirsrc.iso" rc=0
  # QEMU is given absolute paths: it does not run from here, and $OUT may
  # or may not be absolute already.
  local abs="$src" absiso="$iso"
  case "$abs" in /*) ;; *) abs="$PWD/$abs"; absiso="$PWD/$absiso";; esac
  target/release/discx mktree "$src" || { echo "mktree failed"; return 1; }
  target/release/discx export "isodir:$src" "$iso" >/dev/null || { echo "export failed"; return 1; }
  [ -d "$ext" ] && chmod -R u+w "$ext"; rm -rf "$ext"; mkdir -p "$ext"
  # Both readers are independent of us; xorriso is preferred only because
  # libarchive rewrites names to NFD on macOS, which no guest does.
  local extra=()
  if command -v xorriso >/dev/null; then
    xorriso -osirrox on -indev "$iso" -extract / "$ext" >"$OUT/dirdisc-extract.log" 2>&1 || { echo "xorriso could not read the volume"; return 1; }
  elif command -v bsdtar >/dev/null; then
    bsdtar -xf "$iso" -C "$ext" >"$OUT/dirdisc-extract.log" 2>&1 || { echo "bsdtar could not read the volume"; return 1; }
    [ "$OS" = Darwin ] && extra=(-x 'caf*')
  else
    echo "needs xorriso or bsdtar"; return 77
  fi
  chmod -R u+w "$ext"
  # The two names Joliet cannot hold are excluded here and checked below:
  # everything else must come back exactly as it went in.
  diff -r -x 'semi*' -x 'star*' "${extra[@]}" "$src" "$ext" || { echo "the folder did not come back identical"; rc=1; }
  cmp -s "$src/semi;colon.txt" "$ext/semi_colon.txt" || { echo "semi;colon.txt is not there as semi_colon.txt"; rc=1; }
  cmp -s "$src/star*name.txt" "$ext/star_name.txt" || { echo "star*name.txt is not there as star_name.txt"; rc=1; }

  # The block layer: the same bytes through QEMU's own read path.
  if [ -x build/qemu/qemu-img ]; then
    local info; info="$(build/qemu/qemu-img info --output=json "isodir:$abs" 2>/dev/null)"
    echo "$info" | grep -q '"format": "isodir"' || { echo "not opened by the isodir driver"; echo "$info"; rc=1; }
    build/qemu/qemu-img convert -O raw "isodir:$abs" "$OUT/dirsrc-qemu.iso" 2>/dev/null \
      && cmp -s "$OUT/dirsrc-qemu.iso" "$iso" || { echo "qemu-img read the folder differently from discx"; rc=1; }
  fi

  # The drive: the ATAPI path has to find the disc model through whatever
  # node graph the block layer built — a protocol driver reached by its
  # filename prefix ends up under a probed `raw` format node, and a
  # cdimage_disc() that misses it fails silently, leaving the guest with
  # QEMU's stock answers. CDIMAGE_TRACE prints a line per packet only
  # when the model is there, so SeaBIOS probing the drive is the proof;
  # the same run on a plain .iso (the raw driver, no model) is the control.
  if [ -x build/qemu/qemu-system-i386 ]; then
    local n c
    n="$(atapi_disc_packets "isodir:$abs" "$OUT/dirdisc-probe.log")"
    c="$(atapi_disc_packets "$absiso" "$OUT/dirdisc-control.log")"
    [ "${n:-0}" -gt 0 ] || { echo "the drive saw no disc model for the folder (cdimage_disc found nothing)"; rc=1; }
    [ "${c:-0}" = 0 ] || { echo "the trace fired for a plain .iso on the raw driver: it proves nothing"; rc=1; }
  fi
  return $rc
}

atapi_disc_packets() { # disc, log -> packets the cdimage disc path saw while SeaBIOS probed the drive
  local disc="$1" out="$2" pid i
  CDIMAGE_TRACE=1 build/qemu/qemu-system-i386 -L qemu/pc-bios -machine pc -m 64 \
    -display none -net none -boot d -no-reboot \
    -debugcon "file:$out.bios" -global isa-debugcon.iobase=0x402 \
    -drive "if=none,id=cd0,media=cdrom,file=$disc" \
    -device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0 >"$out" 2>&1 &
  pid=$!
  # SeaBIOS says this once it has probed every drive; it takes well under
  # a second, and the machine would otherwise sit there with no disk.
  for i in $(seq 1 40); do
    grep -q "No bootable device" "$out.bios" 2>/dev/null && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.25
  done
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  grep -c "atapi-disc:" "$out" 2>/dev/null || true
}

cdimage_check() { # the block driver through qemu-img / qemu-io on the selftest images
  local d="$OUT/disc" want=$((6800 * 2048)) rc=0
  for f in mixed.cue mixed.ccd cooked.cue; do
    local info; info="$(build/qemu/qemu-img info --output=json "$d/$f")" || { echo "$f: qemu-img info failed"; return 1; }
    echo "$info" | grep -q '"format": "cdimage"' || { echo "$f: not probed as cdimage"; echo "$info"; rc=1; }
    echo "$info" | grep -q "\"virtual-size\": $want" || { echo "$f: virtual size is not $want"; echo "$info"; rc=1; }
  done
  build/qemu/qemu-img info --output=json "$d/plain.iso" | grep -q '"format": "raw"' || { echo "plain.iso: no longer probes to raw"; rc=1; }
  build/qemu/qemu-img dd -f cdimage -O raw bs=2048 count=2000 "if=$d/mixed.cue" "of=$OUT/cdimage-dd.bin" || rc=1
  cmp "$OUT/cdimage-dd.bin" "$d/plain.iso" || { echo "the data track through the block layer differs from plain.iso"; rc=1; }
  local o
  o="$(build/qemu/qemu-io -r -c "read $((1000 * 2048)) 2048" "$d/lec.cue" 2>&1)"
  case "$o" in *"read failed"*) ;; *) echo "the flipped sector read cleanly"; rc=1;; esac
  o="$(build/qemu/qemu-io -r -c "read $((2000 * 2048)) 2048" "$d/mixed.cue" 2>&1)"
  case "$o" in *"read failed"*) ;; *) echo "an audio sector read as data"; rc=1;; esac
  [ "$(nm -D build/qemu/libqemu-embed-i386.$SO 2>/dev/null | grep -c ' T _ZN3std')" = 0 ] || { echo "Rust std symbols exported from the embed library"; rc=1; }
  return $rc
}
host_check_probe() { # `launcher --host-check` (ADR-013), on any host
  local rc=0 o
  # This host's own answer: either verdict is legal, the report is not.
  o="$(target/release/launcher --host-check 2>&1)" || true
  case "$o" in *"Vulkan loader:"*) ;; *) echo "the report names no loader"; echo "$o"; rc=1;; esac
  case "$o" in *"Required: a 1.3 device"*) ;; *) echo "the report names no bar"; echo "$o"; rc=1;; esac
  # A host with no Vulkan driver at all, which every host can be made
  # into: both loader variables, since which one is read depends on how
  # old the loader is.
  o="$(VK_DRIVER_FILES=/nonexistent.json VK_ICD_FILENAMES=/nonexistent.json \
       target/release/launcher --host-check 2>&1)" \
    && { echo "exit 0 with no Vulkan driver"; rc=1; }
  case "$o" in *unavailable*) ;; *) echo "no Vulkan driver, yet not reported unavailable"; echo "$o"; rc=1;; esac
  case "$o" in *WineD3D*) ;; *) echo "no Vulkan driver, yet not pointed at WineD3D"; echo "$o"; rc=1;; esac
  # Where lavapipe is installed, the other half is testable for real: a
  # software driver is *usable* (DXVK ranks a CPU device last but never
  # excludes it), so the verdict is available and the warning is that it
  # will be slow — never a refusal (ADR-013).
  local lvp; lvp="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)"
  if [ -n "$lvp" ]; then
    o="$(VK_DRIVER_FILES="$lvp" target/release/launcher --host-check 2>&1)" \
      || { echo "a software driver was refused instead of warned about"; echo "$o"; rc=1; }
    case "$o" in *slow*) ;; *) echo "a software driver was not called slow"; echo "$o"; rc=1;; esac
    case "$o" in *"software, usable but slow"*) ;; *) echo "the software device was not listed as usable"; echo "$o"; rc=1;; esac
  fi
  return $rc
}
optimizations_check() { # the wizard's fast-path switches, all the way to a real QEMU
  local rc=0 dir="$OUT/opt-switches" bundle args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$(target/release/launcher --new xp opts "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  # A machine nobody has touched must produce the command line it always
  # produced: no properties, and no `[optimizations]` table in the file.
  args="$(target/release/launcher --print-args "$bundle")"
  case "$args" in *-cpu\ pentium3\ *) ;; *) echo "a default machine names a CPU property"; echo "$args"; rc=1;; esac
  case "$args" in *=on*|*=off*) echo "a default machine names an optimization"; echo "$args"; rc=1;; esac
  grep -q '^\[optimizations\]' "$bundle" && { echo "a default machine wrote an [optimizations] table"; rc=1; }
  # Every switch, through the real form: off where it ships on, on where
  # it ships off, and each on the option QEMU looks it up on — a CPU
  # property on `-cpu`, an accelerator property on `-accel tcg`.
  target/release/launcher --optimizations "$bundle" \
    x87-fast off sse-fast off simd-fast off rep-fast off \
    smc-same-value off inline-lookup off pinned-regs on >"$OUT/optimizations-set.log" 2>&1 \
    || { echo "--optimizations failed"; cat "$OUT/optimizations-set.log"; rc=1; }
  args="$(target/release/launcher --print-args "$bundle")"
  for p in x87-fast=off sse-fast=off simd-fast=off rep-fast=off; do
    case "$args" in *"-cpu pentium3,"*"$p"*) ;; *) echo "$p is not on -cpu"; echo "$args"; rc=1;; esac
  done
  for p in smc-same-value=off inline-lookup=off pinned-regs=on; do
    case "$args" in *"-accel tcg,"*"$p"*) ;; *) echo "$p is not on -accel tcg"; echo "$args"; rc=1;; esac
  done
  # The point of the whole thing: our QEMU accepts the line the launcher
  # writes. Started paused on the real binary and told to quit, so a
  # rejected property is an exit code and not a hung guest.
  if [ -x build/qemu/qemu-system-i386 ] && [ -x build/qemu/qemu-img ]; then
    # A real image, because a zero-byte file is not a disk; and
    # `audiodev=embed0` is the player's own backend, which lives inside
    # the embed library and not out here, so a null one takes the name
    # (the same stand-in `tools/dos-guest-test.py` uses) and the
    # machine's device line is run verbatim.
    build/qemu/qemu-img create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
         | timeout 30 build/qemu/qemu-system-i386 $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the launcher's command line"; echo "$o" | tail -3; rc=1; }
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  # "All defaults" empties the table again rather than writing every
  # switch out at its shipped value.
  target/release/launcher --optimizations "$bundle" defaults >/dev/null 2>&1 || rc=1
  grep -q '^\[optimizations\]' "$bundle" \
    && { echo "\"All defaults\" left an [optimizations] table behind"; rc=1; }
  return $rc
}
have_display() { [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ] || [ "$OS" = Darwin ]; }
preview_anim_check() { # the shader preview keeps drawing (doc 07)
  # Plenty of presets do not stand still: an interlaced CRT draws
  # alternate fields, a phosphor afterglow decays, an NTSC signal
  # shimmers. The editor's preview renders on demand, so unless it knows
  # to keep asking it shows one frozen frame of all that — the bug this
  # guards. Both front ends take the answer from `launcher_core::preview`,
  # so it is asked here through the verb they share.
  local moving=third_party/slang-shaders/crt/crt-beans-vga.slangp
  local still=third_party/slang-shaders/crt/crt-lottes.slangp
  local rc=0
  # A preset that stands still: said to stand still, and the same picture
  # at any frame number. Also the probe — a box with no usable GPU can
  # answer none of this, and that is a skip, not a failure.
  if ! PREVIEW_FRAME=0 target/release/launcher --preview-shader \
       "$still" "$GOLDEN" "$OUT/preview-still-0.png" >"$OUT/preview-still.txt" 2>&1; then
    sed 's/^/  /' "$OUT/preview-still.txt"
    echo "no usable GPU for a headless preview"
    return 77
  fi
  grep -qx still "$OUT/preview-still.txt" || { echo "$still: reported as animated"; rc=1; }
  PREVIEW_FRAME=7 target/release/launcher --preview-shader \
    "$still" "$GOLDEN" "$OUT/preview-still-7.png" >>"$OUT/preview-still.txt" 2>&1 || rc=1
  cmp -s "$OUT/preview-still-0.png" "$OUT/preview-still-7.png" \
    || { echo "$still: frames 0 and 7 differ — the frame number reaches a preset that does not read it"; rc=1; }
  # A preset that does not: said to animate, and two frame numbers really
  # are two pictures (this one interlaces, so it is half the frame).
  PREVIEW_FRAME=0 target/release/launcher --preview-shader \
    "$moving" "$GOLDEN" "$OUT/preview-moving-0.png" >"$OUT/preview-moving.txt" 2>&1 || rc=1
  grep -qx animated "$OUT/preview-moving.txt" || { echo "$moving: reported as still"; rc=1; }
  PREVIEW_FRAME=1 target/release/launcher --preview-shader \
    "$moving" "$GOLDEN" "$OUT/preview-moving-1.png" >>"$OUT/preview-moving.txt" 2>&1 || rc=1
  if cmp -s "$OUT/preview-moving-0.png" "$OUT/preview-moving-1.png"; then
    echo "$moving: frames 0 and 1 are the same picture — the preview would be frozen"
    rc=1
  fi
  return $rc
}

# ---------------------------------------------------------------- host stage
host_stage() {
  log "host stage ($OS $ARCH)"

  # x87 oracle: only an x86 host has the real x87 to compare against
  if [ "$ARCH" = x86_64 ] || [ "$ARCH" = i686 ]; then
    cc -O2 -std=gnu11 -Iqemu/target/i386/tcg -o build/x87-fast-test tools/x87-fast-test.c -lm \
      && run_check x87-fast x87-fast.log build/x87-fast-test 2000000 \
      || { [ -x build/x87-fast-test ] || { FAIL+=(x87-fast); echo "  FAIL x87-fast (build)"; }; }
  else
    skip x87-fast "x87 oracle needs an x86 host"
  fi

  # the CD-ROM model (M5, doc 17): images written and read back by discx
  cargo build --release -p libdisc -q 2>"$OUT/libdisc-build.log" \
    && run_check libdisc libdisc.log target/release/discx selftest "$OUT/disc" \
    || { [ -x target/release/discx ] || { FAIL+=(libdisc); echo "  FAIL libdisc (build)"; }; }
  if [ -x build/qemu/qemu-img ] && [ -f "$OUT/disc/mixed.cue" ]; then
    run_check cdimage cdimage.log cdimage_check || true
  else skip cdimage "needs build/qemu/qemu-img and the libdisc check's images"; fi
  if [ -x target/release/discx ]; then
    run_check dirdisc dirdisc.log dirdisc_check || true
  else skip dirdisc "needs target/release/discx"; fi

  # the host GPU probe (ADR-013): what the launcher tells someone about 3D
  # before a machine exists. The verdict itself is a property of the box,
  # so what is checked here is the part that has to hold on every box —
  # that a host with no Vulkan driver at all is reported unavailable,
  # exits non-zero and is pointed at the WineD3D path, that a software
  # driver is warned about rather than refused, and that a report always
  # names the loader and the bar it was judged against.
  cargo build --release -p launcher -q 2>"$OUT/host-check-build.log" \
    && run_check host-check host-check.log host_check_probe \
    || { [ -x target/release/launcher ] || { FAIL+=(host-check); echo "  FAIL host-check (build)"; }; }

  # the wizard's emulation-optimization switches (patches/qemu/README.md):
  # that a machine nobody has touched still produces the command line it
  # always produced, that each switch lands on the option QEMU looks it up
  # on — a CPU property on `-cpu`, an accelerator property on `-accel tcg`
  # — and that our own QEMU actually accepts the line the launcher writes.
  # The switches' *effect* is the guest batteries' job (x87-guest,
  # sse-guest, rep-guest, smc-guest); this is the wiring between them and
  # a checkbox.
  cargo build --release -p launcher -q 2>"$OUT/optimizations-build.log" \
    && run_check optimizations optimizations.log optimizations_check \
    || { [ -x target/release/launcher ] || { FAIL+=(optimizations); echo "  FAIL optimizations (build)"; }; }

  # the Linux package (M6 step 6): staged from this build and asked, with a
  # scrubbed environment, whether it resolves its own player, qemu-img,
  # firmware and guest-tools — the launcher's paths are otherwise baked in
  # at compile time and a regression there only shows on someone else's
  # machine. Rolls no tarball (the check is the point, not the archive).
  if [ "$OS" = Linux ] && [ -f build/qemu/libqemu-embed-i386.so ] && [ -x build/qemu/qemu-img ] && [ -d qemu/pc-bios ]; then
    run_check package package.log scripts/package-linux.sh --no-tar --out "$OUT/package" || true
  else
    skip package "Linux with build/qemu (libqemu-embed, qemu-img) and qemu/pc-bios only"
  fi

  # the C ABI (doc 07): `launcher-core` is a library, and this proves it is
  # usable as one — a C program creating a DOS machine through the shared
  # wizard, putting a disc on the shelf and reading both back. It is the
  # only check on the *third* front end's surface, so a rename or a
  # changed default in a model shows up here as well as in the two GUIs.
  # A scratch library and shelf, never the user's own.
  if cargo build -p launcher-capi >"$OUT/capi-build.log" 2>&1; then
    CAPI_LIB=""
    for cand in target/debug/liblauncher_capi.a target/release/liblauncher_capi.a; do
      [ -f "$cand" ] && CAPI_LIB="$cand" && break
    done
    if [ -n "$CAPI_LIB" ] && cc -O1 -std=gnu11 -Ilauncher-capi/include \
         -o build/capi-smoke launcher-capi/examples/smoke.c "$CAPI_LIB" \
         -lstdc++ -lm -ldl -lpthread >>"$OUT/capi-build.log" 2>&1; then
      rm -rf "$OUT/capi"; mkdir -p "$OUT/capi/library"
      : >"$OUT/capi/disc.iso"
      run_check capi capi.log env \
        LAUNCHER_LIBRARY_DIR="$OUT/capi/library" \
        LAUNCHER_DISC_LIBRARY="$OUT/capi/discs.toml" \
        LAUNCHER_SHADER_PROFILES_DIR="$OUT/capi/profiles" \
        build/capi-smoke "$OUT/capi/library" "$OUT/capi/disc.iso" || true
    else
      FAIL+=(capi); echo "  FAIL capi (build)"
    fi
  else
    FAIL+=(capi); echo "  FAIL capi (cargo build -p launcher-capi)"
  fi

  # the embed library's Mesa backend, Linux (EGL) only, one VM per process
  if [ "$OS" = Linux ] && [ -f build/qemu/libqemu-embed-i386.so ]; then
    if cc -O1 -std=gnu11 -Iembed -o build/embed-3d-test tools/embed-3d-test.c \
         -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,"$ROOT/build/qemu" -lepoxy; then
      run_check embed-3d embed-3d.log build/embed-3d-test || true
    else FAIL+=(embed-3d); echo "  FAIL embed-3d (build)"; fi
  else
    skip embed-3d "Linux with build/qemu/libqemu-embed-i386.so only"
  fi

  # Glide pass-through without a guest: the real host-side wrapper, loaded
  # by hw/3dfx's own dispatcher, rendering into the window-less context.
  # Linux (EGL) only, one VM per process, like embed-3d above.
  if [ "$OS" = Linux ] && [ -f build/qemu/libqemu-embed-i386.so ] \
     && [ -f build/glide/libglide2x.so ]; then
    if c++ -O1 -std=c++17 -w -Iembed -Ithird_party/openglide -Iqemu/hw/3dfx \
         -o build/glide-host-test tools/glide-host-test.cpp \
         -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,"$ROOT/build/qemu" -ldl; then
      QEMU_GLIDE_LIB="$ROOT/build/glide/libglide2x.so" \
        GLIDE_TEST_BMP="$OUT/glide-frame.bmp" \
        run_check glide-host glide-host.log build/glide-host-test || true
    else FAIL+=(glide-host); echo "  FAIL glide-host (build)"; fi
  else
    skip glide-host "Linux with build/glide/libglide2x.so only (scripts/build-glide.sh)"
  fi

  # decoder + executor without a guest
  if [ -f "$D3DPT_EXEC_LIB" ] && [ -f "$D3DPT_DXVK_LIB" ]; then
    if c++ -std=c++17 -O2 -o build/d3dpt-exec-test tools/d3dpt-exec-test.cpp \
         -I"$DX" -I"$DX/windows" -I"$DX/directx" -ldl; then
      run_check d3dpt-exec d3dpt-exec.log build/d3dpt-exec-test "$OUT/exec-test.bmp" 120 60 || true
    else FAIL+=(d3dpt-exec); echo "  FAIL d3dpt-exec (build)"; fi
    if c++ -std=c++17 -O2 -o build/d3dpt-dp2-test tools/d3dpt-dp2-test.cpp \
         -I"$DX" -I"$DX/windows" -I"$DX/directx" -ldl; then
      run_check d3dpt-dp2 d3dpt-dp2.log build/d3dpt-dp2-test "$OUT/dp2-test.bmp" || true
    else FAIL+=(d3dpt-dp2); echo "  FAIL d3dpt-dp2 (build)"; fi
  else
    skip d3dpt-exec "needs $D3DPT_EXEC_LIB and $D3DPT_DXVK_LIB"
    skip d3dpt-dp2 "needs $D3DPT_EXEC_LIB and $D3DPT_DXVK_LIB"
  fi

  # the calibration patterns (doc 09): they render at every era mode, and the
  # circle in `grid` comes out round on the tube it is drawn for
  if cc -O2 -w -o build/crtcal-render tools/crtcal-render.c -lm; then
    mkdir -p "$OUT/crtcal"
    run_check crtcal crtcal.log build/crtcal-render "$OUT/crtcal" || true
  else FAIL+=(crtcal); echo "  FAIL crtcal (build)"; fi

  # the player's display path without a guest: mode analysis, the geometry
  # stage and the CRT preset over every mode in the table (doc 03, M2)
  local preset=third_party/slang-shaders/crt/crt-guest-advanced.slangp
  if have_display && [ -f "$preset" ]; then
    if cargo build --release -p player -q 2>"$OUT/player-build.log"; then
      run_check mode-sweep mode-sweep.log \
        target/release/player --shader "$preset" --mode-sweep "$OUT/mode-sweep" || true
    else FAIL+=(mode-sweep); echo "  FAIL mode-sweep (build)"; tail -5 "$OUT/player-build.log"; fi
  else
    skip mode-sweep "needs a display and the slang-shaders submodule"
  fi

  # the launcher's shader preview, which unlike the player renders only
  # when asked: that it knows which presets it must keep asking about,
  # and that a frame number really does change their picture (doc 07)
  if [ -f third_party/slang-shaders/crt/crt-beans-vga.slangp ] && [ -x target/release/launcher ]; then
    run_check preview-anim preview-anim.log preview_anim_check || true
  else
    skip preview-anim "needs the slang-shaders submodule and target/release/launcher"
  fi

  # the reference scene and the feature test natively over DXVK (SDL2 needs a display)
  if [ -f "$D3DPT_DXVK_LIB" ] && have_display && pkg-config --exists sdl2; then
    local flags=(-I"$DX" -I"$DX/windows" -I"$DX/directx" -Lbuild/dxvk/src/d3d9 -ldxvk_d3d9 \
                 -Wl,-rpath,"$ROOT/build/dxvk/src/d3d9" $(pkg-config --cflags --libs sdl2))
    if c++ -std=c++17 -O2 -o build/d3dgame9-native tools/d3dgame9-native.cpp "${flags[@]}" \
       && c++ -std=c++17 -O2 -o build/d3dfeat9-native tools/d3dfeat9-native.cpp "${flags[@]}"; then
      rm -f "$OUT/d3dgame9.log" "$OUT/d3dfeat9.log"
      ( cd "$OUT" && DXVK_WSI_DRIVER="${DXVK_WSI_DRIVER:-SDL2}" ../d3dgame9-native -frames 600 -dump 300 g9-native.bmp ) >"$OUT/d3dgame9-native.log" 2>&1
      if [ -f "$OUT/g9-native.bmp" ]; then
        run_check d3dgame9-nat d3dgame9-golden.log tools/bmpdiff.py "$GOLDEN" "$OUT/g9-native.bmp" \
          --mask "$HUD_MASK" --tolerance 8 --max-over "$BUDGET" -o "$OUT/g9-native-vs-rig.bmp" \
          && sed -n 1,2p "$OUT/d3dgame9-golden.log" | sed 's/^/       /'
      else FAIL+=(d3dgame9-nat); echo "  FAIL d3dgame9-nat (no frame) — $OUT/d3dgame9-native.log"; tail -3 "$OUT/d3dgame9-native.log"; fi
      ( cd "$OUT" && DXVK_WSI_DRIVER="${DXVK_WSI_DRIVER:-SDL2}" ../d3dfeat9-native -frames 600 -dump 300 f9-native.bmp ) >"$OUT/d3dfeat9-native.log" 2>&1
      if [ -f "$OUT/f9-native.bmp" ] && grep -q "occlusion query" "$OUT/d3dfeat9.log"; then
        PASS+=(d3dfeat9-nat); echo "  PASS d3dfeat9-nat"
        grep "occlusion query\|getters" "$OUT/d3dfeat9.log" | sed 's/^/       /'
      else FAIL+=(d3dfeat9-nat); echo "  FAIL d3dfeat9-nat — $OUT/d3dfeat9-native.log"; tail -3 "$OUT/d3dfeat9-native.log"; fi
    else FAIL+=(d3d-native); echo "  FAIL d3d native harness (build)"; fi
  else
    skip d3dgame9-nat "needs build/dxvk, sdl2 and a display"
    skip d3dfeat9-nat "needs build/dxvk, sdl2 and a display"
  fi
}

# --------------------------------------------------------------- guest stage
QEMU_PID=""; SOCK=""
qmp() { python3 tools/qmpc.py "$SOCK" "$@" >/dev/null; }
guest_teardown() {
  [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null || return 0
  if [ "${TEST_KEEP:-0}" = 1 ]; then echo "  TEST_KEEP=1: XP left running, QMP at $SOCK (pid $QEMU_PID)"; return 0; fi
  qmp json '{"execute":"system_powerdown"}' 2>/dev/null
  for _ in $(seq 60); do kill -0 "$QEMU_PID" 2>/dev/null || return 0; sleep 2; done
  echo "  XP did not power down, killing"; kill "$QEMU_PID" 2>/dev/null
}
guest_stage() {
  log "guest stage"
  local img="${WINXP_IMG:-$HOME/vms/winxp.qcow2}"
  local iso="${GUEST_ISO:-$(ls -t guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)}"
  if command -v nasm >/dev/null && command -v mcopy >/dev/null && [ -x build/qemu/qemu-system-i386 ]; then
    if [ -f build/images/144m/x86BOOT.img ]; then
      run_check x87-guest x87-guest.log python3 tools/x87-guest-test.py || true
      run_check rep-guest rep-guest.log python3 tools/rep-guest-test.py || true
      run_check smc-guest smc-guest.log python3 tools/smc-guest-test.py || true
      run_check sse-guest sse-guest.log python3 tools/sse-guest-test.py || true
      run_check atapi-guest atapi-guest.log python3 tools/atapi-guest-test.py || true
    else skip x87-guest "no FreeDOS floppy yet: run tools/x87-guest-test.py once to fetch it"; fi
  else skip x87-guest "needs nasm, mtools and build/qemu"; fi
  if [ "$OS" != Linux ]; then skip guest "Linux only for now (mkfs.fat, sfdisk, mtools)"; return; fi
  for t in mkfs.fat sfdisk mcopy mmd; do command -v $t >/dev/null || { skip guest "needs $t"; return; }; done
  [ -f "$img" ] || { skip guest "no XP image at $img (WINXP_IMG)"; return; }
  [ -n "$iso" ] && [ -f "$iso" ] || { skip guest "no guest-tools ISO (guest-tools/build-wrappers.sh)"; return; }
  [ -x build/qemu/qemu-system-i386 ] || { skip guest "no build/qemu/qemu-system-i386"; return; }
  # the CD-ROM backend: XP copies a converted guest-tools disc through cdrom.sys (doc 17 §6.3)
  if [ -x target/release/discx ] && command -v bsdtar >/dev/null; then
    # bsdtar keeps the ISO's read-only modes: make the previous extraction deletable first
    [ -d "$OUT/gt-iso" ] && chmod -R u+w "$OUT/gt-iso"
    rm -rf "$OUT/gt-iso"; mkdir -p "$OUT/gt-iso" "$OUT/disc"
    if bsdtar -xf "$iso" -C "$OUT/gt-iso" 2>/dev/null && chmod -R u+w "$OUT/gt-iso" \
       && target/release/discx convert "$iso" "$OUT/disc/gt.cue" --audio "$OUT/disc/tone.wav" >/dev/null 2>&1; then
      # with mingw the run also plays the tone track through MCI into a wav (CD-DA, doc 17 §5.4)
      cdtest=""
      if command -v i686-w64-mingw32-gcc >/dev/null && i686-w64-mingw32-gcc -O2 -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
           -march=pentium3 -mtune=generic -o "$OUT/CDTEST.EXE" guest-tools/src/cdtest.c -lwinmm 2>"$OUT/cdtest-build.log"; then
        cdtest="$OUT/CDTEST.EXE"
      else echo "  (no mingw: guest-cdimage runs without the CD audio part)"; fi
      CDTEST="$cdtest" run_check guest-cdimage guest-cdimage.log tools/xp-cdimage-test.sh "$img" "$OUT/disc/gt.cue" "$OUT/gt-iso" "$OUT/cdimage-xp" || true
    else skip guest-cdimage "could not extract or convert $iso"; fi
  else skip guest-cdimage "needs target/release/discx and bsdtar"; fi
  [ -f "$D3DPT_EXEC_LIB" ] || { skip guest "no $D3DPT_EXEC_LIB"; return; }
  [ -f "$OUT/g9-native.bmp" ] && [ -f "$OUT/f9-native.bmp" ] || { skip guest "run the host stage first (native oracle frames)"; return; }
  local accel="${TEST_ACCEL:-}"
  if [ -z "$accel" ]; then if [ -w /dev/kvm ]; then accel=kvm; else accel=tcg; fi; fi
  local cpu=(-cpu pentium3); [ "$accel" = kvm ] && cpu=(-cpu host)

  # scratch disk: 64 MB FAT32, one partition at 2048, RUN.BAT drives the whole session
  local scratch="$OUT/scratch.img"
  rm -f "$scratch"; truncate -s 64M "$scratch"
  printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$scratch" >/dev/null
  mkfs.fat -F 32 --offset 2048 "$scratch" >/dev/null
  local fat="$scratch@@1048576"
  printf '@echo off\r\nxcopy D:\\D3DPT E:\\D3DPT\\ /I /Y\r\nxcopy D:\\TESTS E:\\D3DPT\\ /I /Y\r\nmkdir E:\\OUT\r\ncd /d E:\\D3DPT\r\nDDVMTEST.EXE\r\nD3DGAME9.EXE -frames 600 -dump 300 E:\\OUT\\G9.BMP\r\nD3DGAME8.EXE -frames 600 -dump 300 E:\\OUT\\G8.BMP\r\nD3DFEAT9.EXE -frames 600 -dump 300 E:\\OUT\\F9.BMP\r\necho done > E:\\OUT\\DONE.TXT\r\n' > "$OUT/RUN.BAT"
  mcopy -i "$fat" "$OUT/RUN.BAT" ::/RUN.BAT

  SOCK="$OUT/qmp.sock"; rm -f "$SOCK"
  local qlog="$OUT/qemu.log"
  echo "  XP: $img (snapshot), ISO: $iso, accel: $accel"
  build/qemu/qemu-system-i386 -L qemu/pc-bios -accel "$accel" "${cpu[@]}" -machine pc -m 512 \
    -drive "file=$img,if=ide,index=0,media=disk,snapshot=on" -drive "file=$scratch,format=raw,if=ide,index=1,media=disk" -cdrom "$iso" \
    -vga cirrus -net none -usb -device usb-tablet -display none -serial none -monitor none \
    -qmp "unix:$SOCK,server,nowait" >"$qlog" 2>&1 &
  QEMU_PID=$!
  trap guest_teardown EXIT
  for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
  [ -S "$SOCK" ] || { FAIL+=(guest-boot); echo "  FAIL guest-boot (no QMP socket) — $qlog"; tail -5 "$qlog"; return; }

  # boot: XP autologs in; the Run dialog is retried until the device sees the first attach
  local deadline=$(( $(date +%s) + ${TEST_BOOT_TIMEOUT:-300} )) t0=$(date +%s)
  sleep 25
  while ! grep -q ", attached (" "$qlog"; do
    if [ "$(date +%s)" -ge "$deadline" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
      FAIL+=(guest-boot); echo "  FAIL guest-boot (no d3dpt attach within the timeout) — $qlog"; tail -5 "$qlog"; return; fi
    qmp keys meta_l+r; sleep 1; qmp keys ctrl+a; qmp type 'E:\RUN.BAT'; qmp keys ret
    for _ in $(seq 15); do grep -q ", attached (" "$qlog" && break; sleep 1; done
  done
  echo "  first attach after $(( $(date +%s) - t0 )) s"
  # three programs, three detaches; then DONE.TXT once XP has flushed the FAT
  deadline=$(( $(date +%s) + 300 ))
  while [ "$(grep -c "DLL_PROCESS_DETACH" "$qlog")" -lt 3 ]; do
    if [ "$(date +%s)" -ge "$deadline" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
      FAIL+=(guest-run); echo "  FAIL guest-run ($(grep -c "DLL_PROCESS_DETACH" "$qlog") of 3 programs detached) — $qlog"; grep -i "d3dpt:" "$qlog" | tail -8; return; fi
    sleep 2
  done
  for _ in $(seq 30); do mcopy -n -i "$fat" ::/OUT/DONE.TXT "$OUT/DONE.TXT" 2>/dev/null && break; sleep 2; done
  echo "  guest run done after $(( $(date +%s) - t0 )) s; shutting XP down"
  QEMU_KEEP="${TEST_KEEP:-0}"; TEST_KEEP=0 guest_teardown; TEST_KEEP="$QEMU_KEEP"; QEMU_PID=""
  rm -f "$OUT"/G9.BMP "$OUT"/G8.BMP "$OUT"/F9.BMP "$OUT"/guest-*.log
  mcopy -n -i "$fat" ::/OUT/G9.BMP ::/OUT/G8.BMP ::/OUT/F9.BMP "$OUT/" 2>/dev/null
  mcopy -n -i "$fat" ::/D3DPT/d3dgame9.log "$OUT/guest-d3dgame9.log" 2>/dev/null
  mcopy -n -i "$fat" ::/D3DPT/d3dgame8.log "$OUT/guest-d3dgame8.log" 2>/dev/null
  mcopy -n -i "$fat" ::/D3DPT/d3dfeat9.log "$OUT/guest-d3dfeat9.log" 2>/dev/null
  mcopy -n -i "$fat" ::/D3DPT/ddvmtest.log "$OUT/guest-ddvmtest.log" 2>/dev/null
  # the DirectDraw shim next to the EXE: a Vice City-style launcher check passes
  if grep -q "ddraw.dll is E:" "$OUT/guest-ddvmtest.log" 2>/dev/null && grep -q ": enough" "$OUT/guest-ddvmtest.log"; then
    PASS+=(guest-ddvm); echo "  PASS guest-ddvm"; grep "GetAvailableVidMem" "$OUT/guest-ddvmtest.log" | tr -d '\r' | sed 's/^/       /'
  else FAIL+=(guest-ddvm); echo "  FAIL guest-ddvm — $OUT/guest-ddvmtest.log"; cat "$OUT/guest-ddvmtest.log" 2>/dev/null | tr -d '\r' | sed 's/^/       /'; fi

  local f
  for f in G9 G8; do
    if [ ! -f "$OUT/$f.BMP" ]; then FAIL+=("guest-$f"); echo "  FAIL guest-$f (no frame on the scratch disk)"; continue; fi
    run_check "guest-$f=native" "guest-$f-native.log" tools/bmpdiff.py "$OUT/g9-native.bmp" "$OUT/$f.BMP" --mask "$HUD_MASK" || true
    run_check "guest-$f~rig" "guest-$f-rig.log" tools/bmpdiff.py "$GOLDEN" "$OUT/$f.BMP" --mask "$HUD_MASK" --tolerance 8 --max-over "$BUDGET" || true
  done
  if [ ! -f "$OUT/F9.BMP" ]; then FAIL+=(guest-F9); echo "  FAIL guest-F9 (no frame on the scratch disk)"
  else
    run_check "guest-F9=native" guest-F9-native.log cmp "$OUT/f9-native.bmp" "$OUT/F9.BMP" || true
    grep -h "occlusion query\|getters" "$OUT/d3dfeat9.log" | sort > "$OUT/f9-native.lines"
    grep -h "occlusion query\|getters" "$OUT/guest-d3dfeat9.log" 2>/dev/null | tr -d '\r' | sort > "$OUT/f9-guest.lines"
    run_check "guest-F9-log=native" guest-F9-log.log diff "$OUT/f9-native.lines" "$OUT/f9-guest.lines" || true
  fi
}

case "$STAGE" in
  host) host_stage;;
  guest) guest_stage;;
  all) host_stage; guest_stage;;
  *) echo "usage: $0 [host|guest|all]"; exit 2;;
esac

printf '\n%d passed, %d failed, %d skipped\n' ${#PASS[@]} ${#FAIL[@]} ${#SKIP[@]}
for s in "${SKIP[@]}"; do echo "  skip: $s"; done
for f in "${FAIL[@]}"; do echo "  FAIL: $f"; done
[ ${#FAIL[@]} = 0 ]
