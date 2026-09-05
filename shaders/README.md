# shaders

Curated slang preset pack (design doc 03): "Trinitron" (calibrated against
the reference CRT, doc 09), "shadow mask consumer", "clean sharp". Populated
in M2. Presets reference upstream libretro slang-shaders; only our `.slangp`
parameter files and any custom passes live here.

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
profiles entirely.
