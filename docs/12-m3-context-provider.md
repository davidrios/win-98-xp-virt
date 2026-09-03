# 12. M3 design: window-less GL context provider for qemu-3dfx

Status 2026-09-02: steps 1–2 done on Linux — patch 30 (vtable), patch 31
(weak native backend), `embed/mglcntx_embed.c` (EGL surfaceless, pbuffer =
FBO 0, glReadPixels on swap), `embed/embedfx.c` (provider), embed API v4
(`on_3d_active`, `on_3d_frame`), player publishes 3D frames on swap.
Verified without a guest by `tools/embed-3d-test.c` (drives the backend in
mesapt_mm.c's order: activation callbacks, 640×480 frame, correct
orientation) and with the real thing: Win98 wglgears in the player on the
Linux dev box — 420 fps at 800×600 through the readback, VGA desktop back
on exit. macOS backend done and verified on the Air (CGL context without a
drawable + FBO stand-in for the default framebuffer, binding 0 redirected
via patch 32's `MesaGLSetFunc`; WGL pbuffers emulated with FBOs in the same
context; all GL/CGL resolved through the framework handle because the
build also links XQuartz's Mesa libGL): Win98 wglgears in the player,
`GL 2.1 Metal / Apple M1`. **Steps 1–2 complete on both platforms.**
**Zero-copy on Linux done (2026-09-03):** the backend allocates a ring of
three linear ARGB8888 GBM buffers, imports them into GL as EGLImage
textures and blits FBO 0 into the next one (Y-flipped) on every swap;
each buffer's dma-buf is offered once (embed API v5 `on_3d_dmabuf`) and
the player imports it into wgpu through `wgpu-hal` Vulkan
(`VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`,
`player/src/dmabuf.rs`); per frame only the slot index travels
(`on_3d_frame_ready`). Frames are sampled straight from the guest's
buffers: Win98 wglgears 575–600 fps (was 420–450 with readback). Sync is
`glFinish` before the hand-off for now (a fence fd is the refinement).
`tools/embed-3d-test.c` checks the ring by mmap'ing the dma-bufs; the
readback path stays as the fallback (macOS, or no Vulkan extensions).
Next: IOSurface on macOS (§4), then Glide (§5).

Source survey of the patched tree (hw/mesa, hw/3dfx, ui/sdl2.c); file:line
refs are to `qemu/` as prepared by `scripts/prepare-qemu.sh`.

## Facts that shape the design

- **All host GL runs on the vCPU thread under the BQL.** The Mesa device is
  an MMIO handler (`mesapt_mm.c:2213`); every GL call, context creation
  (`MGLCreateContext`) and present (`MGLSwapBuffers`) happens there. The
  main-loop timer callbacks that touch GL (`sched_wndproc`,
  `deactivateOneshot`, `dispTimerProc`) also hold the BQL. There is no GL
  thread; the BQL is the mutex. The SDL glue creates the context on the main
  loop and *unbinds* it (`ui/sdl2.c:931`) so the vCPU can claim it.
- **The guest frame lives in FBO 0 of the window's default framebuffer.**
  `MesaBlitScale` (`mesagl_blit.c:228-321`) upscales by `glCopyTexImage2D`
  from FBO 0 and redraws; `MesaRenderScaler` rewrites viewports only when
  `framebuffer_binding == 0`. Keep "FBO 0 = the screen" semantics: make FBO
  0 itself offscreen (pbuffer / CGL pbuffer or IOSurface drawable) rather
  than binding an app FBO.
- **Compatibility profile is mandatory** — guests are GL 1.1–2.1
  fixed-function. QEMU's own `qemu_egl_init_ctx()` hardcodes the core
  profile (`ui/egl-helpers.c:613`), so we create our own context. On macOS
  compat GL is 2.1 only (`ui/sdl2.c:877-880` core/compat switch via
  `DispTimerMS`).
- **The UI seam is exactly 11 functions** declared in
  `include/ui/console.h:481-494` and defined only in `ui/sdl2.c`:
  `mesa_{prepare,release}_window`, `mesa_renderer_stat`,
  `mesa_gui_fullscreen`, `mesa_cursor_define`, `mesa_mouse_warp`,
  `glide_{prepare,release}_window`, `glide_window_stat`,
  `glide_gui_fullscreen`, `glide_renderer_stat`. The handshake: vCPU calls
  `mesa_prepare_window(msaa, alpha, 0, cwnd_fn)`; the provider must
  eventually call `cwnd_fn(swnd, nwnd, opaque)`, which sets `wnd_ready`;
  the guest spins on MMIO `0xFB8` (`glwnd_ready()`) until then.
  `mesa_gui_fullscreen(sizev)` is called on every swap and must be
  synchronous: `sizev[0..1]` = guest 2D surface size, `sizev[2..3]` = target
  size; making them equal disables the in-QEMU scaler (we scale in wgpu).
- **Backends are GLX (`mglcntx_linux.c`, Linux) and SDL/native
  (`mglcntx_sdlgl.c`, macOS)**; `MGLCreateContext/MGLMakeCurrent/
  MGLSwapBuffers` are the only window-system-specific parts; the rest
  (`MGLFuncHandler`, pbuffer emulation, `MGLUpdateGuestBufo`) is reusable.
  `MesaGLGetProc` resolves extensions (`glXGetProcAddress` /
  `SDL_GL_GetProcAddress`); the base table comes from `dlopen(libGL)`.
- **3D-active signalling** is `graphic_hw_passthrough(con, on)`
  (`ui/console.c:129`), which makes `graphic_hw_update` skip the VGA
  device. Nothing tells a display listener; `con->ui_info.passthrough` is
  private. We add an explicit edge notification to the embed API.
- **Glide presents inside a third-party wrapper** (`libglide2x.so` creates
  its own context on the window handle, `glide2x_impl.c:796-806`). Defer.

## Design

1. **Patch `30-3dfx-ui-vtable`:** turn the 11 entry points into a vtable
   registered by the active display (`ui/sdl2.c` registers the SDL one at
   init; default = "no provider": Mesa refuses contexts as in patch 04,
   Glide reports). Fixes the latent NULL derefs in `sdl_gui_fullscreen` /
   `sdl_renderer_stat` too.
2. **`embed/mglcntx_embed.c`** (selected by meson instead of the GLX/SDL
   backend when building the embed library): copy of the GLX backend with
   the window-system calls replaced —
   - Linux: EGL on a DRM render node (reuse `egl_rendernode_init`), own
     context with `EGL_OPENGL_API` + compatibility profile, **EGL pbuffer**
     drawable at guest resolution so FBO 0 stays valid; after
     `MesaBlitScale`, blit into an exportable texture and
     `egl_get_fd_for_texture()` → dma-buf.
   - macOS: CGL (not NSOpenGL — no main-thread requirement). Implemented
     without a drawable at all: an FBO over shared renderbuffers plays the
     default framebuffer and `glBindFramebuffer(…, 0)` is redirected to it
     in the dispatch table (CGL pbuffers are deprecated; `FBO 0 = the
     screen` semantics survive because the scaler is inert at equal sizes).
     Legacy (2.1 compatibility) profile by default; core 3.2/4.1 when the
     guest asks via `wglCreateContextAttribsARB`. The IOSurface export
     attaches an IOSurface-backed texture to that FBO later.
   - `MGLSwapBuffers` = flush + fence + publish handle + notify; never block
     the vCPU on the consumer. 2–3 buffer ring with fences.
   - Delete `XOpenDisplay(NULL)`; `MesaGLGetProc` → `eglGetProcAddress` /
     CGL symbol lookup.
3. **`embed/ui_embed_ctx.c`:** the 11 entry points for the embed provider:
   `mesa_prepare_window` creates the drawable on the main loop (BH), calls
   `cwnd_fn`, unbinds; `mesa_gui_fullscreen` returns target == guest size
   at first (scaler inert), `mesa_cursor_define`/`mouse_warp` unchanged but
   sourced from `qemu_console_lookup_by_index(0)`; `renderer_stat` keeps
   `graphic_hw_passthrough` and raises the new embed callback.
4. **Embed API v4:** `on_3d_active(bool)` and `on_3d_frame(handle, w, h,
   fence)` display callbacks; handle = dma-buf fd (Linux) / IOSurface id
   (macOS). Player: import into wgpu via `wgpu-hal` (Vulkan external memory
   / Metal IOSurface) and feed the librashader chain instead of the VGA
   texture while 3D is active. Step 0 for bring-up: a CPU readback path
   (`glReadPixels` into the staging buffer) to validate the whole handshake
   before the zero-copy import — explicitly a stepping stone (doc 03's
   "readback is the failure line" is about the shipped design).
5. **Glide later:** needs either an offscreen-capable wrapper build (new
   signature word next to `'SDL2'`) or a hidden SDL window + readback.

## Order

vtable patch → embed provider on Linux with readback → dma-buf import →
macOS CGL/IOSurface → Glide.
