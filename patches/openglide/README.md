# OpenGLide patch queue

Applied by `scripts/prepare-openglide.sh` on top of the pinned OpenGLide
submodule (`third_party/openglide`, the CVS mirror at `ad9a3dd`) in filename
order; the script restores every tracked file a patch touches and re-applies
the queue on each run, like `prepare-qemu.sh` and `prepare-dxvk.sh`.

## Why OpenGLide is here at all

qemu-3dfx's Glide pass-through (`hw/3dfx`) is a *dispatcher*, not an
implementation: at `grGlideInit` it `dlopen`s a host-side `libglide2x` and
looks up 183 `gr…`/`gu…` entry points in it. That library is not part of
qemu-3dfx — upstream ships it to donors only ("QEMU-enhanced OpenGLide
host-side wrappers"). Without one, Glide has never worked here in any
configuration, the player and `-display sdl` alike.

OpenGLide is the open implementation, and the one upstream's is derived from
(its `setConfig(FxU32)` with the `WRAPPER_FLAG_*` bits `glidewnd.c` sets is
already here; kjliew's fork widened it to `_setConfig@8`). Of the 183 entry
points it defines **121**, which is every one Glide 2.x needs —
`grSstWinOpen`, `grBufferSwap`, `grDrawTriangle`, `grTexDownloadMipMap`,
`grLfbLock`/`Unlock`, `guTexAllocateMemory`. The 62 it lacks are Glide 3
(`grDrawVertexArray`, `grVertexLayout`, `grGet`…), the Voodoo3/Napalm `…Ext`
extensions, and the Glide 2.11 LFB API — so `glide3x.dll` guests and a
handful of late titles are out of scope until someone adds them.

The build is `scripts/build-glide.sh`, which compiles the sources directly
rather than carrying OpenGLide's autotools: its `configure` looks for SDL
1.2, X11 and GLU, all three of which this build exists to do without. The
window-less platform layer, the entry points `hw/3dfx` looks for and the two
GLU replacements live in `glidept/host/`, outside the submodule; only what
had to change *inside* OpenGLide is a patch.

**macOS builds the same sources** (2026-09-06). OpenGLide says `<GL/gl.h>`
and `<GL/glext.h>` outright -- `platform/window.h` reaches for the framework
only under `__MACOSX__`, an SDL-era define nothing sets -- and macOS has no
`GL/` directory: the framework keeps its headers under `OpenGL/`, and the
only `GL/` on the box belongs to XQuartz's Mesa, the one implementation this
must not bind to (`docs/build-macos.md`). So `glidept/host/macos/GL/` holds
a forwarding `gl.h` and `glext.h`, and `build-glide.sh` puts that directory
on the include path **on Darwin only** -- a Linux build still finds the real
headers. The `glext.h` carries what Apple's copy lacks: it stopped at
`GL_GLEXT_VERSION 8` (2003), before the `PFNGL…PROC` convention, so the
seventeen typedefs OpenGLide names (it resolves every extension through the
platform's `GetProcAddress`), `APIENTRY`, and the four
`EXT_paletted_texture` / `EXT_packed_pixels` enums `PGTexture.cpp` names
behind a runtime check for extensions macOS does not have. Nothing inside
the submodule changed for it, so it is a directory rather than a patch. The
result links against `OpenGL.framework` and `libSystem` alone -- 120
`gr…`/`gu…` exports plus `setConfig` -- but **nothing exercises it there**:
`glide-host` drives the embed backend's EGL path and stays Linux-only, and
the guest run below is a Linux one, so the macOS wrapper is built, not
proven.

| Patch | What / why | Drop when |
|---|---|---|
| `01-no-glu` | drop `<GL/glu.h>` and the two GLU calls: `gluErrorString` (one log line) and `gluBuild2DMipmaps` (behind the off-by-default `BuildMipMaps`). Replaced by `ogl_error_string` / `ogl_build_2d_mipmaps` in `glidept/host/glu_shim.cpp` — a switch statement and `GL_GENERATE_MIPMAP`, which is the driver's own downsample rather than GLU's box filter and correct here because Glide textures are already power-of-two. GLU is deprecated and absent from runtimes we ship into (`org.freedesktop.Sdk`) | never, unless GLU comes back |
| `02-host-entry-points` | the wrapper must be safe to load *inside QEMU*. Three things: (a) `GlideMsg`/`Error`/`ClearAndGenerateLogFile`/`GenerateErrorFile` write through one switch, `GLIDE_HOST_LOG=<path>` (`-` = stderr) — upstream writes `OpenGLid.log` and `OpenGLid.err` into the working directory from a **static constructor** and returns a failure the caller answers with `exit(0)`, which inside a VM process is neither wanted nor survivable, and re-`fopen`s the log per message; (b) `GetOptions` no longer *writes* an `OpenGLid.ini` when none is found — the working directory is the player's, not a game folder — it just keeps its defaults, and still reads one that exists; (c) upstream's one-argument `setConfig` is removed and its declaration widened to qemu-3dfx's `_setConfig@8` shape, because the replacement (with `setConfigRes` and `setHostOps`) is in `glidept/host/hostops.cpp` | upstream grows a library-friendly logger |
| `03-sdk-header-in-c` | `sdk2_3dfx.h` is the public Glide SDK header, and a **C** program includes it too (`guest-tools/src/glidetest.c`, the guest-side test): `#include <cstdint>` and `#define FX_ENTRY extern "C"` are both C++-only, and the second is a syntax error on every declaration in the file. Both now branch on `__cplusplus` | upstream notices |
| `04-lfb-origin` | `grLfbLock` fills `lfbPtr`, `writeMode` and `strideInBytes` of the caller's `GrLfbInfo_t` but never `origin`, which is an out field too: the caller reads back whatever was already in its own struct. It costs qemu-3dfx a warning per lock (`LFB origin mismatch` in the QEMU log, found by the first Glide guest run, 2026-09-06) and it is not only cosmetic — the dispatcher caches the value in `lfbDev->origin`, and a Glide **2.11** title's `grLfbBegin` is answered from that cache, so one lock would leave an old game reading its buffer upside down. The rows are already laid out for the origin that was asked for, so the fix is to say so | upstream notices |

## Regenerating

Edit inside `third_party/openglide`, then `git -C third_party/openglide diff
-- <files>` for the patch's own hunks, and prove it forward-applies from
pristine by running `scripts/prepare-openglide.sh` twice and rebuilding.
Both patches touch `GLutil.cpp`, in disjoint hunks; that is why the GLU one
is first.

## The stacks we did not take

Two other ways a guest's Glide call could reach the GPU, recorded so they
are not re-argued:

- **A guest-side Glide→OpenGL wrapper** (OpenGLide's own Win32 build) on top
  of qemu-3dfx's `OPENGL32.DLL` pass-through. Needs no host code at all and
  would work today — but every Glide call is translated by guest code under
  TCG before it becomes a GL call that then crosses the MMIO boundary,
  instead of crossing once as a Glide call and being translated at host
  speed. Avoiding exactly that is why qemu-3dfx has a Glide device.
- **A guest-side Glide→Direct3D wrapper** (nGlide, dgVoodoo2) on top of our
  own paravirtual D3D (doc 14) or the XP driver's DX8 DDI (doc 15). The
  architecture is sound and the translation would run at host speed in
  DXVK — but both are closed-source freeware, so neither can go on the
  guest-tools ISO or into a Flatpak. Secondary: dgVoodoo2 wants D3D11 and
  nGlide's D3D path is XP-shaped, which misses Win98, where the Glide
  titles are.
