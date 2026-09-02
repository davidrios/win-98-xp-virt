# Spike A: qemu-3dfx GL output → wgpu texture (macOS first)

Go/no-go for the 3D-through-CRT-shader path under ADR-005. Test machines:
the M1 MacBook Air (Metal) and the Arch box (Vulkan). Result recorded at the
bottom and in doc 10.

## Question

qemu-3dfx renders guest Glide/GL with host OpenGL on QEMU's threads. Can that
output land in a **wgpu texture** on the player's render thread — no CPU
readback — so it goes through librashader like the 2D framebuffer does?

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

_Not yet run._
