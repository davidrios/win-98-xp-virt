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
