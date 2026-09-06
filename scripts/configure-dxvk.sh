#!/usr/bin/env bash
# meson setup for the native DXVK d3d9 library (the D3D executor, doc 14 /
# ADR-007) into build/dxvk. Only d3d9 is built; the SDL2 WSI is the stand-in
# until the window-less embed WSI exists. Then: ninja -C build/dxvk
#   macOS: brew install vulkan-headers vulkan-loader glslang sdl2 meson ninja
#          (+ the LunarG SDK for the KosmicKrisp ICD on macOS 26)
#   Arch:  pacman -S vulkan-headers vulkan-icd-loader glslang sdl2 meson ninja
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build/dxvk}"
if [ "$(uname -s)" = Darwin ]; then
  export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi
opts=(--buildtype release -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false
      -Dnative_sdl2=enabled -Dnative_glfw=disabled -Dnative_sdl3=disabled)
# A meson build directory holds absolute paths and cannot be relocated, so
# a renamed or moved checkout (2ksbox, 2026-09-06) leaves one whose
# --reconfigure walks into directories that no longer exist: "[Errno 2] No
# such file or directory: <old checkout>/build/dxvk/meson-private/tmp…".
# Wipe and configure from scratch when the reconfigure fails, whatever the
# reason — a real error (a missing Vulkan header, say) simply fails again
# on the fresh configure, with its own message.
if [ -f "$BUILD/build.ninja" ]; then
  meson setup --reconfigure "$BUILD" "$ROOT/third_party/dxvk" "${opts[@]}" || {
    echo "==> reconfigure failed; configuring $BUILD from scratch"
    rm -rf "$BUILD"
    meson setup "$BUILD" "$ROOT/third_party/dxvk" "${opts[@]}"
  }
else
  meson setup "$BUILD" "$ROOT/third_party/dxvk" "${opts[@]}"
fi
echo "==> ninja -C $BUILD"
