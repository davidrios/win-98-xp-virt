#!/usr/bin/env bash
# Build libd3dpt_exec (d3dpt/exec: the paravirtual Direct3D device's decoder
# + DXVK executor, doc 14) into build/d3dpt. Needs build/dxvk (headers only:
# the library dlopens libdxvk_d3d9 at runtime). Linux: .so, macOS: .dylib.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/d3dpt"; mkdir -p "$OUT"
DX="$ROOT/third_party/dxvk/include/native"
if [ "$(uname -s)" = Darwin ]; then LIB="$OUT/libd3dpt_exec.dylib"; SHARED=(-dynamiclib -install_name "$LIB"); else LIB="$OUT/libd3dpt_exec.so"; SHARED=(-shared); fi
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -O2 -fPIC -fvisibility=hidden -Wall -Wno-unused-function "${SHARED[@]}" -o "$LIB" \
  "$ROOT/d3dpt/exec/d3dpt_exec.cpp" "$ROOT/d3dpt/exec/d3dpt_exec_ddi.cpp" -I"$DX" -I"$DX/windows" -I"$DX/directx" -ldl
echo "==> $LIB"
