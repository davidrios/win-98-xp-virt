# Spike A: qemu-3dfx GL output → wgpu texture (macOS first)

Go/no-go for the 3D-through-CRT-shader path under ADR-005. Test machines:
the M1 MacBook Air (Metal) and the Arch box (Vulkan). Result recorded at the
bottom and in doc 10.

## Question

qemu-3dfx renders guest Glide/GL with host OpenGL on QEMU's threads. Can that
output land in a **wgpu texture** on the player's render thread — no CPU
readback — so it goes through librashader like the 2D framebuffer does?

## Finding (2026-09-02, verified against the built objects)

`hw/mesa` ships two Unix context backends, but the overlay's `meson.build`
compiles **only `mglcntx_linux.c`** (GLX); `mglcntx_sdlgl.c` is dead code
in this build. On Darwin that GLX backend dlopens
`/opt/X11/lib/libGL.dylib` — so at runtime guest 3D lands in an **XQuartz
GLX drawable**, and XQuartz is a hard runtime dependency, not just a
build one. (SDL2 is still required by the patched build; the 3dfx Glide
window path uses it.)

**Hard constraint (confirmed 2026-09-02 on the Air):** 3D activation
requires QEMU's SDL2 display — `sdl_display_valid()` (patched `ui/sdl2.c`)
exits unless `sdl2_console` exists, and the SDL window is torn down and
recreated to host the GL context (`sdl_gui_restart`). With the player's
`-display none`, launching a GL/Glide title would `exit(1)` the whole
process. So the M3 integration is not optional plumbing: our fork must
replace the SDL-window dependency (`mesa_prepare_window`,
`glide_prepare_window`, `sdl_display_valid`, fullscreen helpers in
`include/ui/console.h:482-490`) with a window-less context provider that
renders to a texture we can import.

Consequences for the interop:
- Step 2 must establish what XQuartz's libGL sits on (Apple's GLX bridge
  over native OpenGL). IOSurface sharing via the underlying CGL context
  may be reachable, but through an extra layer.
- The cleaner path for us is likely a **native CGL (or SDL2) context
  backend for `hw/mesa` on Darwin in our fork** — `mglcntx_sdlgl.c` already
  exists as a starting point and targets `OpenGL.framework` directly —
  which drops XQuartz entirely and makes the IOSurface route direct.
  Scope after step 2.
- Upstream Darwin support is evidently lightly maintained: the July 2026
  sync broke the Darwin build (`GL_CONTEXTALPHA`), fixed in our queue by
  `patches/qemu/00-3dfx-darwin-contextalpha.patch`.

## Sub-questions, cheapest first

1. **Does qemu-3dfx run on the M1 Air at all?** Use the startergo
   `qemu-3dfx-macos` arm64 build first, then our own build
   (`scripts/prepare-qemu.sh` + `configure-qemu.sh` with brew deps). Boot a
   Win98 guest with the guest wrappers; confirm accelerated GL (renderer
   string) in QEMU's own window. Validates the plain macOS path.
2. **Where does qemu-3dfx's output live?** Read `hw/mesa` / `hw/3dfx`
   host-side code: does it render into its own window/context, an FBO, or
   blit to the VGA surface? The answer decides whether we intercept at an
   FBO/texture or need to add a render-target hook in our fork.
3. **Interop mechanism per platform:**
   - macOS: GL texture backed by an **IOSurface** → `MTLTexture` from the
     same IOSurface → wgpu `Texture` via `wgpu-hal` Metal import.
   - Linux: GL `EXT_memory_object_fd` / dma-buf ↔ Vulkan external memory
     → wgpu-hal Vulkan import.
   - Windows: `WGL_NV_DX_interop2` or shared handle ↔ D3D12.
   Throwaway hack: a standalone program (no QEMU) that draws with GL into
   the shared surface and displays it through the player's wgpu pipeline.
4. **Synchronization:** fence/semaphore between QEMU's GL thread and the
   render thread (or a copy into a triple buffer on the GL side, then
   import). A texture copy is acceptable; a readback is the failure line.

## Fallback ladder

a. GL-side copy into an imported shared texture (cost: one GPU copy). →
b. ANGLE (GL-on-Metal) under qemu-3dfx so everything is Metal. →
c. Zink (GL-on-Vulkan) on Linux/Windows for a single-API path. →
d. 3D bypasses the shader chain (own GL window) — last resort, 2D still
   correct.

## Result

- **2026-09-02 — build validated on the M1 Air:** our patched QEMU 9.2.4
  (qemu-3dfx overlay + `00-3dfx-darwin-contextalpha` patch, XQuartz + SDL2
  from Homebrew, uv Python) compiles, runs, and exposes
  `glidept`/`glidelfb`/`glideshm`/`mesapt` via `info mtree`. Steps 1–4
  (guest 3D, interop) not yet run.
