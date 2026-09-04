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
#   embed-3d       tools/embed-3d-test.c: the window-less Mesa backend (Linux)
#   d3dpt-exec     tools/d3dpt-exec-test.cpp: guest encoder → decoder → DXVK,
#                  frames delivered, hostile batch refused
#   d3dgame9-nat   the reference scene natively on DXVK: frame 300 within
#                  D3D_GOLDEN_BUDGET pixels (tolerance 8, HUD masked) of the
#                  rig golden; this frame is the oracle for the guest stage
#   d3dfeat9-nat   the feature test natively: frame + log lines kept as the
#                  oracle for the guest stage
#
# Guest stage
#   sse-guest      tools/sse-guest-test.py: the SSE battery, sse-fast on/off identical (doc 16)
#   x87-guest      tools/x87-guest-test.py: a DOS x87 battery under TCG,
#                  identical with the fast path on and off (needs nasm,
#                  mtools and the FreeDOS floppy the tool fetches on first use)
#   XP (Linux; KVM when /dev/kvm is usable, TCG otherwise):
#   boots WINXP_IMG read-only (snapshot=on) with the newest guest-tools ISO
#   and a fresh FAT32 scratch disk carrying RUN.BAT, drives the Run dialog
#   over QMP, waits for the three programs to detach from the device, shuts
#   XP down, and diffs: D3DGAME9 and D3DGAME8 pixel-identical to the native
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
have_display() { [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ] || [ "$OS" = Darwin ]; }

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

  # the embed library's Mesa backend, Linux (EGL) only, one VM per process
  if [ "$OS" = Linux ] && [ -f build/qemu/libqemu-embed-i386.so ]; then
    if cc -O1 -std=gnu11 -Iembed -o build/embed-3d-test tools/embed-3d-test.c \
         -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,"$ROOT/build/qemu" -lepoxy; then
      run_check embed-3d embed-3d.log build/embed-3d-test || true
    else FAIL+=(embed-3d); echo "  FAIL embed-3d (build)"; fi
  else
    skip embed-3d "Linux with build/qemu/libqemu-embed-i386.so only"
  fi

  # decoder + executor without a guest
  if [ -f "$D3DPT_EXEC_LIB" ] && [ -f "$D3DPT_DXVK_LIB" ]; then
    if c++ -std=c++17 -O2 -o build/d3dpt-exec-test tools/d3dpt-exec-test.cpp \
         -I"$DX" -I"$DX/windows" -I"$DX/directx" -ldl; then
      run_check d3dpt-exec d3dpt-exec.log build/d3dpt-exec-test "$OUT/exec-test.bmp" 120 60 || true
    else FAIL+=(d3dpt-exec); echo "  FAIL d3dpt-exec (build)"; fi
  else
    skip d3dpt-exec "needs $D3DPT_EXEC_LIB and $D3DPT_DXVK_LIB"
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
      run_check sse-guest sse-guest.log python3 tools/sse-guest-test.py || true
    else skip x87-guest "no FreeDOS floppy yet: run tools/x87-guest-test.py once to fetch it"; fi
  else skip x87-guest "needs nasm, mtools and build/qemu"; fi
  if [ "$OS" != Linux ]; then skip guest "Linux only for now (mkfs.fat, sfdisk, mtools)"; return; fi
  for t in mkfs.fat sfdisk mcopy mmd; do command -v $t >/dev/null || { skip guest "needs $t"; return; }; done
  [ -f "$img" ] || { skip guest "no XP image at $img (WINXP_IMG)"; return; }
  [ -n "$iso" ] && [ -f "$iso" ] || { skip guest "no guest-tools ISO (guest-tools/build-wrappers.sh)"; return; }
  [ -x build/qemu/qemu-system-i386 ] || { skip guest "no build/qemu/qemu-system-i386"; return; }
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
  printf '@echo off\r\nxcopy D:\\D3DPT E:\\D3DPT\\ /I /Y\r\nmkdir E:\\OUT\r\ncd /d E:\\D3DPT\r\nD3DGAME9.EXE -frames 600 -dump 300 E:\\OUT\\G9.BMP\r\nD3DGAME8.EXE -frames 600 -dump 300 E:\\OUT\\G8.BMP\r\nD3DFEAT9.EXE -frames 600 -dump 300 E:\\OUT\\F9.BMP\r\necho done > E:\\OUT\\DONE.TXT\r\n' > "$OUT/RUN.BAT"
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
