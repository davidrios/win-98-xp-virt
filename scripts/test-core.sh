#!/usr/bin/env bash
# Headless smoke test: load the core in RetroArch with null drivers, run N
# frames, exit. Proves the libretro handshake (contentless load, pixel
# format, av_info, retro_run cadence) with no window, audio, or focus
# dependency — works in CI and agent shells. (GL/window runs on Wayland can
# stall on vsync when the window is unfocused/occluded; not a core bug.)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="${1:-$ROOT/target/release/libwin98xp_libretro.so}"
FRAMES="${2:-180}"

command -v retroarch >/dev/null || { echo "retroarch not installed"; exit 1; }
[ -f "$CORE" ] || { echo "core not found: $CORE (cargo build --release)"; exit 1; }

CFG="$(mktemp)"
LOG="$(mktemp)"
trap 'rm -f "$CFG" "$LOG"' EXIT
printf 'video_driver = "null"\naudio_driver = "null"\ninput_driver = "null"\njoypad_driver = "null"\npause_nonactive = "false"\n' > "$CFG"

timeout -s KILL 60 retroarch -L "$CORE" --verbose --appendconfig="$CFG" --max-frames="$FRAMES" >"$LOG" 2>&1

grep -q 'SET_PIXEL_FORMAT: XRGB8888' "$LOG" || { echo "FAIL: pixel format not negotiated"; tail -20 "$LOG"; exit 1; }
grep -q 'Geometry: 640x480' "$LOG" || { echo "FAIL: av_info geometry wrong"; tail -20 "$LOG"; exit 1; }
echo "OK: core loaded contentless, ran $FRAMES frames, exited clean"
