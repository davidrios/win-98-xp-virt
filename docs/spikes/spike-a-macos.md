# Spike A: qemu-3dfx GL under libretro hw-render on macOS (M1 Air)

Go/no-go for ADR-003's riskiest integration. Test machine: the M1 MacBook
Air. Result gets recorded at the bottom and in doc 10.

## Question

Can qemu-3dfx's host-side GL rendering land in a **libretro hw-render GL
context** provided by RetroArch on Apple Silicon macOS — so guest 3D flows
through the slang shader chain?

## Sub-questions, cheapest first

1. **Does RetroArch on Apple Silicon still offer a GL video driver, and do GL
   hw-render cores work under it?** Install RetroArch (official arm64 build),
   set `video_driver = "gl"`, run a known GL hw-render core (mupen64plus-next
   or parallel-n64 in GL mode). If this fails, hw-render GL on macOS is dead
   and we go straight to fallbacks.
2. **Does qemu-3dfx build and run standalone on the M1 Air at all?** Use the
   startergo `qemu-3dfx-macos` arm64 build first (fastest path), then our own
   build from `third_party/qemu-3dfx` + `scripts/prepare-qemu.sh` via brew
   deps. Boot a Win98 guest with the guest wrappers, confirm accelerated GL
   (renderer string) in its own window. This validates the plain macOS path
   independent of libretro.
3. **The real spike:** minimal libretro core (extend `core/`) that requests
   an OpenGL hw-render context and issues GL draws into the provided FBO —
   confirm it renders through RetroArch's shader chain on the Air. No QEMU
   involved yet; this proves the context plumbing we'd hand to qemu-3dfx.
4. **Integration sketch:** qemu-3dfx's GL calls execute on QEMU threads
   against its own context. Map out (on paper + a throwaway hack) whether its
   context can share objects with the libretro context, or whether we blit
   (guest 3D frame → shared texture → core FBO). A copy is acceptable; a
   readback to CPU is the failure line.

## Fallback ladder (if 3 or 4 fails on macOS)

a. Require the GL video driver on macOS only; Metal elsewhere. →
b. Offscreen GL context + IOSurface/texture share into whatever RetroArch
   uses. →
c. ANGLE or Zink under qemu-3dfx's host GL. →
d. 3D bypasses the shader chain on macOS (2D still correct) — last resort,
   revisit standalone player.

## Linux leg (runs on the Arch box, same questions minus Metal)

GL hw-render under RetroArch's gl/glcore driver + our test core — expected to
just work; establishes the reference behavior before touching the Air.

## Result

_Not yet run._
