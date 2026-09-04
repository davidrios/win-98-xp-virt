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
| `04-wsi-headless` | a window-less WSI driver (`src/wsi/headless`, `DXVK_WSI_DRIVER=Headless`): one fake monitor with a D3D9-era mode list, no window is ever valid, `createSurface` fails. With a NULL device window DXVK's d3d9 swapchain creates no presenter and `Present` is a no-op, so the paravirtual device's executor (`d3dpt/exec`) runs entirely off-screen and reads the backbuffer back (`tools/dxvk-d3d9-test.cpp` with `NOWINDOW=1` exercises it) | never: upstream has no headless WSI |
| `05-fill-mode-non-solid-optional` | `fillModeNonSolid` required → optional; `D3D9DeviceEx::BindRasterizerState` clamps `D3DFILL_WIREFRAME`/`D3DFILL_POINT` to solid when the driver lacks it (the only legal `VkPolygonMode` then). KosmicKrisp (SDK 1.4.357.1, Mesa 26.2.99) does not expose it — the one remaining refusal on macOS 26 after patch 02 (2026-09-03); the reference scene never leaves solid fill | KosmicKrisp exposes `fillModeNonSolid` (Metal has line fill; point fill is the likely blocker) |

Building and testing (`tools/dxvk-d3d9-test.cpp`, build line in its header):
`scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk`.
On Linux (Arch, RADV) the queue builds unchanged and both harnesses run with
only `DXVK_WSI_DRIVER=SDL2` set (2026-09-03; patch 01 is inert there).
The native library dlopens SDL2 and the Vulkan loader by bare name, so on
macOS run with `DYLD_LIBRARY_PATH=/opt/homebrew/lib
SDL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.dylib VK_ICD_FILENAMES=<icd>
DXVK_WSI_DRIVER=SDL2`. On MoltenVK 1.4.2 the harness ends with DXVK's
`Skipping: Device does not support required feature 'shaderCullDistance'`
(2026-09-03) — the expected refusal. **KosmicKrisp on macOS 26 (2026-09-03,
Air on 26.6.2, LunarG SDK 1.4.357.1 installed in `~/VulkanSDK`, component
`com.lunarg.vulkan.kosmic`):** with
`VK_ICD_FILENAMES=$HOME/VulkanSDK/1.4.357.1/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json`
(Homebrew's loader is fine) both harnesses pass — `Found device: Apple M1
(KosmicKrisp 26.2.99)`, the triangle BMP is written, and
`d3dgame9-native -frames 600 -dump 300` gives 1095 pixels beyond tolerance 8
/ 16 beyond 32 against the rig golden (Linux RADV: 1089 / 16), 120 fps
windowed. Patch 05 was needed first (`fillModeNonSolid`). Patch 04's headless
WSI works on KosmicKrisp too: `NOWINDOW=1 DXVK_WSI_DRIVER=Headless` writes a
BMP identical to the SDL2 path's (384 vs 106 fps for the 60-frame triangle).
Install recipe in `docs/build-macos.md`.

Regenerating: run `prepare-dxvk.sh`, edit inside `third_party/dxvk`, then
`git -C third_party/dxvk diff -- <files>` (plain a/ b/ prefixes; new files:
`git add -N` first so they appear with `--- /dev/null`). Forward-apply from a pristine
tree before pushing: `prepare-dxvk.sh` twice must succeed.
