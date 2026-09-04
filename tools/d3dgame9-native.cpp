/*
 * d3dgame9-native: guest-tools/src/d3dgame9.c, unmodified, as a native
 * program on DXVK's d3d9 (the D3D executor, ADR-007). Same options, same
 * log, same -dump BMPs → diff against reference/d3d with tools/bmpdiff.py.
 *
 * Build (macOS and Linux alike; the rpath finds the .dylib/.so in build/dxvk):
 *   c++ -std=c++17 -O2 -o build/d3dgame9-native tools/d3dgame9-native.cpp \
 *     -Ithird_party/dxvk/include/native -Ithird_party/dxvk/include/native/windows \
 *     -Ithird_party/dxvk/include/native/directx $(pkg-config --cflags --libs sdl2) \
 *     -Lbuild/dxvk/src/d3d9 -ldxvk_d3d9 -Wl,-rpath,$PWD/build/dxvk/src/d3d9
 * Run (Linux: DXVK_WSI_DRIVER=SDL2 is enough; macOS env as for tools/dxvk-d3d9-test.cpp):
 *   DYLD_LIBRARY_PATH=/opt/homebrew/lib SDL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.dylib \
 *   VK_ICD_FILENAMES=<icd> DXVK_WSI_DRIVER=SDL2 build/d3dgame9-native -frames 600 -dump 300 g9.bmp
 * WIN32SDL_SHOW=1 shows the window (keyboard camera works); default is hidden.
 */
#define COBJMACROS
#include "d3dgame-native/win32_sdl.h"
#include <d3d9.h>
#include "../guest-tools/src/d3dgame9.c"
