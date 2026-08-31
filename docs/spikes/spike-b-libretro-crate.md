# Spike B: rust-libretro crate vs. own `retro_*` shim

## Findings (2026-08-31, desk research)

Candidates and hw-render status:

- **rust-libretro / rust-libretro-sys** (docs.rs/rust-libretro): maintained;
  `-sys` is bindgen output of `libretro.h` (complete API surface);
  the framework layer wraps environment negotiation incl.
  `SET_HW_RENDER`/context types.
- **libretro-core**: typed wrappers + trait/macro export; typed GL symbol
  table resolved from the hw-render callbacks.
- Reference: libretro's OpenGL-core docs — `SET_HW_RENDER` in
  `retro_load_game`, GL2+/GLES2 contexts.

## Tentative verdict (finalize during Spike A step 3)

Adopt **rust-libretro-sys** for the API surface (replaces the hand-written
`core/src/libretro.rs` — no point re-typing `libretro.h`), but **keep our own
thin export/runtime layer** rather than a framework: the frameworks model a
tidy synchronous per-frame core, while ours orchestrates QEMU's own threads
behind the embed API, and owning the `retro_run`/callback glue keeps that
coupling explicit. Revisit only if the hw-render negotiation code we end up
writing converges on what `rust-libretro`'s framework already does.
