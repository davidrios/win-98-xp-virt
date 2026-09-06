# shaders

Curated slang preset pack (design doc 03): a shadow-mask preset derived from
the reference CRT (doc 09), a Trinitron-style one, "clean sharp". Populated in
M2. Presets reference upstream libretro slang-shaders; only our `.slangp`
parameter files and any custom passes live here.

- `syncmaster-753dfx.slangp` — the rig's own monitor, a 17" Samsung DynaFlat
  with a delta dot trio at ≈0.20 mm. Derived from the tube's geometry, *not*
  yet calibrated against it; the file says which value came from where.
  `player --shader shaders/syncmaster-753dfx.slangp --mode-sweep <dir>` renders
  it over every mode in the table.

The upstream preset tree is the `third_party/slang-shaders` submodule
(libretro/slang-shaders, shallow). Try:
`player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- <qemu args>`
(also `crt-geom`, `crt-easymode`, `crt-royale`, `crt-aperture`).

**Shader profiles** (doc 07 settings taxonomy): the launcher's "Shader
profiles…" manager names a preset plus a set of parameter overrides on
top of it (`launcher/src/shader_profile.rs`), stored one per file under
the profile library (`LAUNCHER_SHADER_PROFILES_DIR`, default the platform
data dir's `shader-profiles`). A machine picks a profile by name in the
wizard; the launcher resolves it into the player's own `--shader
--shader-params` (see README.md) when spawning it — hand-written bundles
can still set `machine.toml`'s `shader` field directly instead, bypassing
profiles entirely. The manager also previews a profile live against a
chosen image (a screenshot), rendering the actual filter chain
(`shader-chain/`, shared with the player) inside the launcher's own
window and re-rendering as sliders move.
