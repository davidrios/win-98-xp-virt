# shaders

Curated slang preset pack (design doc 03): "Trinitron" (calibrated against
the reference CRT, doc 09), "shadow mask consumer", "clean sharp". Populated
in M2. Presets reference upstream libretro slang-shaders; only our `.slangp`
parameter files and any custom passes live here.

The upstream preset tree is the `third_party/slang-shaders` submodule
(libretro/slang-shaders, shallow). Try:
`player --shader third_party/slang-shaders/crt/crt-lottes.slangp -- <qemu args>`
(also `crt-geom`, `crt-easymode`, `crt-royale`, `crt-aperture`).
