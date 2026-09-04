# DXVK patch queue

Applied by `scripts/prepare-dxvk.sh` on top of the pinned DXVK submodule
(`third_party/dxvk`, 3.1.0 master d7ac258) in filename order; the script
restores every tracked file a patch touches and re-applies the queue on each
run. DXVK's d3d9 is the host executor of the paravirtual Direct3D device
(doc 14, ADR-006/007) on Linux and macOS; only `libdxvk_d3d9` is built
(`scripts/configure-dxvk.sh` → `build/dxvk`).

| Patch | What / why | Drop when |
|---|---|---|
| `01-native-macos` | build dxvk-native on macOS: the Windows shim (`util_win32_compat.h`: dlopen-backed `LoadLibraryA`/`GetProcAddress`/`FreeLibrary`, GDI/handle stubs) was `#if __unix__`, which Apple clang does not define; `getExePath` via `_NSGetExecutablePath`; one-argument `pthread_setname_np`; Vulkan loader names `libvulkan.1.dylib`/`libvulkan.dylib`; the d3d9 export list as an ld64 `-exported_symbols_list` (`d3d9.exports`, generated from `d3d9.sym`) instead of a GNU version script | upstream accepts a macOS port |
| `02-geometry-shader-optional` | `geometryShader` required → optional. d3d9 never creates one; Vulkan-on-Metal drivers (KosmicKrisp, MoltenVK) have none, and DXVK only uses the flag for the pipeline-stage mask. Must be re-required if d3d10/11 are ever enabled | KosmicKrisp grows geometry shaders, or upstream scopes the requirement per client API |

| `03-portability-enumeration` | enable `VK_KHR_portability_enumeration` when the loader offers it and set `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`: Vulkan-on-Metal ICDs (MoltenVK, KosmicKrisp) are portability implementations the loader hides from apps that don't opt in (`vkCreateInstance` → `VK_ERROR_INCOMPATIBLE_DRIVER`, DXVK: "Failed to create Vulkan instance") | upstream opts in |

Building and testing (`tools/dxvk-d3d9-test.cpp`, build line in its header):
`scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk`.
On Linux (Arch, RADV) the queue builds unchanged and both harnesses run with
only `DXVK_WSI_DRIVER=SDL2` set (2026-09-03; patch 01 is inert there).
The native library dlopens SDL2 and the Vulkan loader by bare name, so on
macOS run with `DYLD_LIBRARY_PATH=/opt/homebrew/lib
SDL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.dylib VK_ICD_FILENAMES=<icd>
DXVK_WSI_DRIVER=SDL2`. On MoltenVK 1.4.2 the harness ends with DXVK's
`Skipping: Device does not support required feature 'shaderCullDistance'`
(2026-09-03) — the expected refusal; KosmicKrisp on macOS 26 is the target.

Regenerating: run `prepare-dxvk.sh`, edit inside `third_party/dxvk`, then
`git -C third_party/dxvk diff -- <files>` (plain a/ b/ prefixes; new files:
`git add -N` first so they appear with `--- /dev/null`). Forward-apply from a pristine
tree before pushing: `prepare-dxvk.sh` twice must succeed.
