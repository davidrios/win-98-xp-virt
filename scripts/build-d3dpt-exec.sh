#!/usr/bin/env bash
# Build libd3dpt_exec (d3dpt/exec: the paravirtual Direct3D device's decoder
# + DXVK executor, doc 14) into build/d3dpt. Needs build/dxvk (headers only:
# the library dlopens libdxvk_d3d9 at runtime). Linux: .so, macOS: .dylib.
#
#   scripts/build-d3dpt-exec.sh             this host
#   scripts/build-d3dpt-exec.sh --windows   cross to build/win/d3dpt/d3dpt_exec.dll
#
# --windows compiles the same two files against mingw's own <windows.h> and
# <d3d9.h> instead of DXVK's native stand-ins for them, and loads plain
# `d3d9.dll` at run time: on Windows the host already has a Direct3D 9, and
# a DXVK build dropped next to the player overrides it for free (the loader
# searches the executable's directory first). Run it inside
# scripts/win-cross.sh — docs/build-windows.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ "${1:-}" = "--windows" ]; then
  OUT="$ROOT/build/win/d3dpt"; mkdir -p "$OUT"
  LIB="$OUT/d3dpt_exec.dll"
  CXX="${CXX:-x86_64-w64-mingw32-g++}"
  command -v "$CXX" >/dev/null || { echo "no $CXX — run this inside scripts/win-cross.sh"; exit 1; }
  # -static-libgcc/-libstdc++: the DLL is loaded by qemu-system.exe, which
  # is a C program, so it must not need the C++ runtime DLLs beside it.
  # __USE_MINGW_ANSI_STDIO: msvcrt's printf has no %zu, and the DP2 trace
  # (doc 15) is written with it.
  "$CXX" -std=c++17 -O2 -fvisibility=hidden -Wall -Wno-unused-function -shared \
    -D__USE_MINGW_ANSI_STDIO=1 -static-libgcc -static-libstdc++ -o "$LIB" \
    "$ROOT/d3dpt/exec/d3dpt_exec.cpp" "$ROOT/d3dpt/exec/d3dpt_exec_ddi.cpp"
  echo "==> $LIB"
  exit 0
fi

OUT="$ROOT/build/d3dpt"; mkdir -p "$OUT"
DX="$ROOT/third_party/dxvk/include/native"
if [ "$(uname -s)" = Darwin ]; then LIB="$OUT/libd3dpt_exec.dylib"; SHARED=(-dynamiclib -install_name "$LIB"); else LIB="$OUT/libd3dpt_exec.so"; SHARED=(-shared); fi
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -O2 -fPIC -fvisibility=hidden -Wall -Wno-unused-function "${SHARED[@]}" -o "$LIB" \
  "$ROOT/d3dpt/exec/d3dpt_exec.cpp" "$ROOT/d3dpt/exec/d3dpt_exec_ddi.cpp" -I"$DX" -I"$DX/windows" -I"$DX/directx" -ldl
echo "==> $LIB"
