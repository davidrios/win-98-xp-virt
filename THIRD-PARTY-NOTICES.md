# Third-party notices

The binaries this project ships are built from its own source plus the
crates listed below and, for the player, QEMU itself. This file exists to
satisfy the attribution terms of those licences (Apache-2.0 §4 in
particular) and to let anyone packaging or redistributing a build see what
is in it without resolving the dependency graph themselves.

Regenerate the listings with `cargo metadata` — they are derived, not
hand-maintained:

```sh
cargo metadata --format-version 1 > /tmp/meta.json
python3 tools/third-party-notices.py /tmp/meta.json player launcher
```

## QEMU

The **player** links `libqemu-embed-<target>` — QEMU (https://www.qemu.org),
**GPL-2.0-only** as a whole, plus this project's patch queue in
`patches/qemu/` and the qemu-3dfx overlay
(https://github.com/kjliew/qemu-3dfx). QEMU's own licence text is in
`qemu/LICENSE`, and the GPLv2 text this project distributes under is
`COPYING`. The **launcher** does not link QEMU.

## Host-side libraries built from `third_party/`

Two libraries are built from vendored source and shipped beside the player.
Neither is linked into anything: QEMU `dlopen`s them at run time.

- **OpenGLide** (https://github.com/voyageur/openglide, the CVS mirror),
  **LGPL-2.1-or-later**, built as `libglide2x` by `scripts/build-glide.sh`
  with the patch queue in `patches/openglide/` and the window-less platform
  layer in `glidept/host/`. It is the host side of qemu-3dfx's Glide
  pass-through, which ships no implementation of its own. Its licence text
  is `third_party/openglide/LICENSE`; the modified sources are the pinned
  submodule plus that patch queue, both in this repository, which is how the
  LGPL's "distribute the modifications" term is met.
- **DXVK** (https://github.com/doitsujin/dxvk), **zlib/libpng**, built as
  `libdxvk_d3d9` by `scripts/configure-dxvk.sh` with the patch queue in
  `patches/dxvk/`. It is the host executor of the paravirtual Direct3D
  device (doc 14). Licence text in `third_party/dxvk/LICENSE`.

## Shader presets

The launcher can download libretro's `slang-shaders`
(https://github.com/libretro/slang-shaders) at the user's request, and the
`third_party/slang-shaders` submodule is the same collection. Those presets
carry their own per-file licences and are neither modified nor redistributed
by this project.

## Fonts

`epaint_default_fonts` (in the launcher) carries **OFL-1.1** and
**Ubuntu-font-1.0** typefaces in addition to its `MIT OR Apache-2.0` code.

## Apache-2.0 components

Several crates are **Apache-2.0 with no alternative licence**: in the
player `winit`, `cpal`, `ab_glyph`, `ab_glyph_rasterizer`,
`owned_ttf_parser`, `codespan-reporting`, `rspirv`, `spirv`, `gethostname`,
`glutin_wgl_sys`, `gl_generator`, `khronos_api`; the launcher adds `glutin`
and its `*_sys` crates, `accesskit_winit`, `unicode-general-category` and
`ring`. `dpi` is `Apache-2.0 AND MIT` — both apply. None of them ships a
`NOTICE` file, so attribution here is the whole of the obligation.

For how those interact with the player's GPL-2.0-only status, see
**ADR-010** in `docs/10-decisions.md`. The launcher and `shader-chain` are
`GPL-2.0-or-later` for that reason (**ADR-009**).

## Crates, by declared licence

### `player` — 337 third-party crates

**MIT OR Apache-2.0** (154): `ahash`, `allocator-api2`, `android-activity`, `android_system_properties`, `arc-swap`, `arrayvec`, `as-raw-xcb-connection`, `ash`, `bitflags`, `bumpalo`, `cc`, `cfg-if`, `chacha20`, `core-foundation`, `core-foundation-sys`, `core-graphics`, `core-graphics-types`, `cpufeatures`, `crc`, `crc-catalog`, `crc32fast`, `crossbeam-deque`, `crossbeam-epoch`, `crossbeam-utils`, `dasp_sample`, `dirs-next`, `dirs-sys-next`, `document-features`, `either`, `errno`, `fdeflate`, `find-msvc-tools`, `fixedbitset`, `flate2`, `futures-core`, `futures-macro`, `futures-task`, `futures-util`, `getrandom`, `glob`, `glslang`, `glslang-sys`, `gpu-allocator`, `half`, `hashbrown`, `hermit-abi`, `image`, `itoa`, `jni`, `jni-macros`, `jni-sys`, `jni-sys-macros`, `jobserver`, `js-sys`, `libc`, `litrs`, `lock_api`, `log`, `memmap2`, `naga`, `naga-types`, `ndk`, `ndk-context`, `ndk-sys`, `num-derive`, `num-traits`, `once_cell`, `parking_lot`, `parking_lot_core`, `percent-encoding`, `petgraph`, `pkg-config`, `png`, `presser`, `proc-macro-crate`, `proc-macro2`, `profiling`, `quote`, `rand`, `rand_core`, `range-alloc`, `raw-window-metal`, `rayon`, `rayon-core`, `regex`, `regex-automata`, `regex-syntax`, `renderdoc-sys`, `rustc_version`, `rustversion`, `scopeguard`, `semver`, `serde`, `serde_core`, `serde_derive`, `serde_json`, `shlex`, `simdutf8`, `smallvec`, `smol_str`, `spirv-cross-sys`, `spirv-cross2`, `spirv-cross2-derive`, `static_assertions`, `syn`, `thiserror`, `thiserror-impl`, `toml_datetime`, `toml_edit`, `toml_parser`, `ttf-parser`, `unicode-segmentation`, `unicode-width`, `unty`, `wasm-bindgen`, `wasm-bindgen-futures`, `wasm-bindgen-macro`, `wasm-bindgen-macro-support`, `wasm-bindgen-shared`, `web-sys`, `web-time`, `weezl`, `wgpu`, `wgpu-core`, `wgpu-core-deps-apple`, `wgpu-core-deps-emscripten`, `wgpu-core-deps-wasm`, `wgpu-core-deps-windows-linux-android`, `wgpu-hal`, `wgpu-naga-bridge`, `wgpu-types`, `windows`, `windows-collections`, `windows-core`, `windows-future`, `windows-implement`, `windows-interface`, `windows-link`, `windows-numerics`, `windows-result`, `windows-strings`, `windows-sys`, `windows-targets`, `windows-threading`, `windows_aarch64_gnullvm`, `windows_aarch64_msvc`, `windows_i686_gnu`, `windows_i686_gnullvm`, `windows_i686_msvc`, `windows_x86_64_gnu`, `windows_x86_64_gnullvm`, `windows_x86_64_msvc`, `x11rb`, `x11rb-protocol`

**MIT** (72): `alsa-sys`, `android-properties`, `array-concat`, `bincode`, `bincode_derive`, `block2`, `bytes`, `calloop`, `calloop-wayland-source`, `cfg_aliases`, `combine`, `crunchy`, `data-encoding`, `dispatch`, `dlib`, `fax`, `libm`, `libredox`, `nom`, `nom_locate`, `objc-sys`, `objc2`, `objc2-app-kit`, `objc2-cloud-kit`, `objc2-contacts`, `objc2-core-data`, `objc2-core-image`, `objc2-core-location`, `objc2-encode`, `objc2-foundation`, `objc2-link-presentation`, `objc2-metal`, `objc2-quartz-core`, `objc2-symbols`, `objc2-ui-kit`, `objc2-uniform-type-identifiers`, `objc2-user-notifications`, `orbclient`, `ordered-float`, `platform-dirs`, `quick-xml`, `redox_syscall`, `redox_users`, `sctk-adwaita`, `simd-adler32`, `slab`, `smithay-client-toolkit`, `strict-num`, `strumbra`, `tiff`, `tracing`, `tracing-attributes`, `tracing-core`, `unsigned-varint`, `vec_extract_if_polyfill`, `virtue`, `wayland-backend`, `wayland-client`, `wayland-csd-frame`, `wayland-cursor`, `wayland-protocols`, `wayland-protocols-plasma`, `wayland-protocols-wlr`, `wayland-scanner`, `wayland-sys`, `winnow`, `x11-dl`, `xcursor`, `xkbcommon-dl`, `xml-rs`, `zigzag`, `zmij`

**MIT/Apache-2.0** (16): `bitflags`, `coreaudio-rs`, `downcast-rs`, `foreign-types`, `foreign-types-macros`, `foreign-types-shared`, `fs2`, `khronos-egl`, `linked-hash-map`, `plain`, `quick-error`, `scoped-tls`, `version_check`, `winapi`, `winapi-i686-pc-windows-gnu`, `winapi-x86_64-pc-windows-gnu`

**Apache-2.0 OR MIT** (15): `atomic-waker`, `autocfg`, `bit-set`, `bit-vec`, `concurrent-queue`, `equivalent`, `indexmap`, `pin-project`, `pin-project-internal`, `pin-project-lite`, `polling`, `portable-atomic`, `portable-atomic-util`, `rustc-hash`, `simd_cesu8`

**Apache-2.0** (12): `ab_glyph`, `ab_glyph_rasterizer`, `codespan-reporting`, `cpal`, `gethostname`, `gl_generator`, `glutin_wgl_sys`, `khronos_api`, `owned_ttf_parser`, `rspirv`, `spirv`, `winit`

**Zlib OR Apache-2.0 OR MIT** (12): `bytemuck`, `bytemuck_derive`, `dispatch2`, `objc2-audio-toolbox`, `objc2-avf-audio`, `objc2-core-audio`, `objc2-core-audio-types`, `objc2-core-foundation`, `objc2-core-graphics`, `objc2-io-surface`, `objc2-metal`, `objc2-quartz-core`

**MPL-2.0 OR GPL-3.0-only** (9): `librashader`, `librashader-cache`, `librashader-common`, `librashader-pack`, `librashader-preprocess`, `librashader-presets`, `librashader-reflect`, `librashader-runtime`, `librashader-runtime-wgpu`

**MIT OR Apache-2.0 OR Zlib** (6): `cursor-icon`, `glow`, `raw-window-handle`, `xkeysym`, `zune-core`, `zune-jpeg`

**Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT** (5): `linux-raw-sys`, `rustix`, `wasi`, `wasip2`, `wit-bindgen`

**Apache-2.0/MIT** (5): `alsa`, `bytecount`, `halfbrown`, `pollster`, `rustc-hash`

**Unlicense OR MIT** (5): `aho-corasick`, `byteorder-lite`, `memchr`, `termcolor`, `winapi-util`

**Zlib** (3): `foldhash`, `slotmap`, `zlib-rs`

**BSD-2-Clause OR Apache-2.0 OR MIT** (2): `zerocopy`, `zerocopy-derive`

**BSD-3-Clause** (2): `tiny-skia`, `tiny-skia-path`

**BSD-3-Clause OR Apache-2.0** (2): `moxcms`, `pxfm`

**BSD-3-Clause OR MIT OR Apache-2.0** (2): `num_enum`, `num_enum_derive`

**Unlicense/MIT** (2): `same-file`, `walkdir`

**(Apache-2.0 OR MIT) AND BSD-3-Clause** (1): `encoding_rs`

**(MIT OR Apache-2.0) AND Unicode-3.0** (1): `unicode-ident`

**0BSD OR MIT OR Apache-2.0** (1): `adler2`

**Apache-2.0 AND MIT** (1): `dpi`

**BSD-2-Clause** (1): `arrayref`

**BSD-2-Clause OR MIT OR Apache-2.0** (1): `mach2`

**CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception** (1): `blake3`

**CC0-1.0 OR MIT-0 OR Apache-2.0** (1): `constant_time_eq`

**ISC** (1): `libloading`

**MIT OR Apache-2.0 OR LGPL-2.1-or-later** (1): `r-efi`

**MIT OR Zlib OR Apache-2.0** (1): `miniz_oxide`

**MPL-2.0** (1): `persy`

**MPL-2.0+** (1): `smartstring`


### `launcher` — 474 third-party crates

**MIT OR Apache-2.0** (206): `accesskit`, `accesskit_atspi_common`, `accesskit_consumer`, `accesskit_macos`, `accesskit_unix`, `accesskit_windows`, `ahash`, `allocator-api2`, `android-activity`, `android_system_properties`, `arboard`, `arc-swap`, `arrayvec`, `as-raw-xcb-connection`, `ash`, `async-broadcast`, `async-recursion`, `async-trait`, `base64`, `bitflags`, `bumpalo`, `cc`, `cfg-if`, `chacha20`, `core-foundation`, `core-foundation-sys`, `core-graphics`, `core-graphics-types`, `cpufeatures`, `crc`, `crc-catalog`, `crc32fast`, `crossbeam-deque`, `crossbeam-epoch`, `crossbeam-utils`, `directories`, `dirs-next`, `dirs-sys`, `dirs-sys-next`, `displaydoc`, `document-features`, `ecolor`, `eframe`, `egui`, `egui-wgpu`, `egui-winit`, `egui_glow`, `either`, `emath`, `enumflags2`, `enumflags2_derive`, `epaint`, `errno`, `euclid`, `fdeflate`, `find-msvc-tools`, `fixedbitset`, `flate2`, `font-types`, `form_urlencoded`, `futures-core`, `futures-io`, `futures-macro`, `futures-task`, `futures-util`, `getrandom`, `glob`, `glslang`, `glslang-sys`, `gpu-allocator`, `half`, `hashbrown`, `hermit-abi`, `hex`, `http`, `httparse`, `idna`, `image`, `itertools`, `itoa`, `jni`, `jni-macros`, `jni-sys`, `jni-sys-macros`, `jobserver`, `js-sys`, `libc`, `litrs`, `lock_api`, `log`, `memmap2`, `naga`, `naga-types`, `ndk`, `ndk-context`, `ndk-sys`, `num-derive`, `num-traits`, `once_cell`, `ordered-stream`, `parking_lot`, `parking_lot_core`, `percent-encoding`, `petgraph`, `piper`, `pkg-config`, `png`, `polycool`, `presser`, `proc-macro-crate`, `proc-macro2`, `profiling`, `quote`, `rand`, `rand_core`, `range-alloc`, `raw-window-metal`, `rayon`, `rayon-core`, `read-fonts`, `regex`, `regex-automata`, `regex-syntax`, `renderdoc-sys`, `rustc_version`, `rustls-pki-types`, `rustversion`, `scopeguard`, `semver`, `serde`, `serde_core`, `serde_derive`, `serde_json`, `serde_repr`, `serde_spanned`, `shlex`, `signal-hook-registry`, `simdutf8`, `skrifa`, `smallvec`, `smol_str`, `spirv-cross-sys`, `spirv-cross2`, `spirv-cross2-derive`, `stable_deref_trait`, `static_assertions`, `syn`, `tar`, `tempfile`, `thiserror`, `thiserror-impl`, `toml`, `toml_datetime`, `toml_edit`, `toml_parser`, `toml_writer`, `ttf-parser`, `unicode-segmentation`, `unicode-width`, `unty`, `ureq`, `ureq-proto`, `url`, `utf8-zero`, `wasm-bindgen`, `wasm-bindgen-futures`, `wasm-bindgen-macro`, `wasm-bindgen-macro-support`, `wasm-bindgen-shared`, `web-sys`, `web-time`, `webbrowser`, `weezl`, `wgpu`, `wgpu-core`, `wgpu-core-deps-apple`, `wgpu-core-deps-emscripten`, `wgpu-core-deps-wasm`, `wgpu-core-deps-windows-linux-android`, `wgpu-hal`, `wgpu-naga-bridge`, `wgpu-types`, `windows`, `windows-collections`, `windows-core`, `windows-future`, `windows-implement`, `windows-interface`, `windows-link`, `windows-numerics`, `windows-result`, `windows-strings`, `windows-sys`, `windows-targets`, `windows-threading`, `windows_aarch64_gnullvm`, `windows_aarch64_msvc`, `windows_i686_gnu`, `windows_i686_gnullvm`, `windows_i686_msvc`, `windows_x86_64_gnu`, `windows_x86_64_gnullvm`, `windows_x86_64_msvc`, `x11rb`, `x11rb-protocol`, `xattr`

**MIT** (95): `android-properties`, `array-concat`, `bincode`, `bincode_derive`, `block2`, `bytes`, `calloop`, `calloop-wayland-source`, `cfg_aliases`, `combine`, `crunchy`, `data-encoding`, `dispatch`, `dlib`, `endi`, `fax`, `glutin-winit`, `harfrust`, `libm`, `libredox`, `memoffset`, `nom`, `nom_locate`, `objc-sys`, `objc2`, `objc2-app-kit`, `objc2-cloud-kit`, `objc2-contacts`, `objc2-core-data`, `objc2-core-image`, `objc2-core-location`, `objc2-encode`, `objc2-foundation`, `objc2-link-presentation`, `objc2-metal`, `objc2-quartz-core`, `objc2-symbols`, `objc2-ui-kit`, `objc2-uniform-type-identifiers`, `objc2-user-notifications`, `orbclient`, `ordered-float`, `phf`, `phf_generator`, `phf_macros`, `phf_shared`, `platform-dirs`, `quick-xml`, `redox_syscall`, `redox_users`, `rfd`, `sctk-adwaita`, `simd-adler32`, `slab`, `smithay-client-toolkit`, `smithay-clipboard`, `strict-num`, `strumbra`, `synstructure`, `tiff`, `tracing`, `tracing-attributes`, `tracing-core`, `uds_windows`, `unsigned-varint`, `vec_extract_if_polyfill`, `virtue`, `wayland-backend`, `wayland-client`, `wayland-csd-frame`, `wayland-cursor`, `wayland-protocols`, `wayland-protocols-experimental`, `wayland-protocols-misc`, `wayland-protocols-plasma`, `wayland-protocols-wlr`, `wayland-scanner`, `wayland-sys`, `winnow`, `x11-dl`, `xcursor`, `xkbcommon-dl`, `xml-rs`, `zbus`, `zbus-lockstep`, `zbus-lockstep-macros`, `zbus_macros`, `zbus_names`, `zbus_xml`, `zcheapstr`, `zigzag`, `zmij`, `zvariant`, `zvariant_derive`, `zvariant_utils`

**Apache-2.0 OR MIT** (44): `async-channel`, `async-executor`, `async-io`, `async-lock`, `async-process`, `async-signal`, `async-task`, `atomic-waker`, `atspi`, `atspi-common`, `atspi-proxies`, `autocfg`, `bit-set`, `bit-vec`, `blocking`, `color`, `concurrent-queue`, `equivalent`, `event-listener`, `event-listener-strategy`, `fastrand`, `fearless_simd`, `futures-lite`, `glifo`, `idna_adapter`, `indexmap`, `kurbo`, `linebender_resource_handle`, `nohash-hasher`, `parking`, `peniko`, `pin-project`, `pin-project-internal`, `pin-project-lite`, `polling`, `portable-atomic`, `portable-atomic-util`, `rustc-hash`, `simd_cesu8`, `utf8_iter`, `uuid`, `vello_common`, `vello_cpu`, `zeroize`

**MIT/Apache-2.0** (19): `bitflags`, `downcast-rs`, `filetime`, `foreign-types`, `foreign-types-macros`, `foreign-types-shared`, `fs2`, `guillotiere`, `khronos-egl`, `linked-hash-map`, `plain`, `quick-error`, `scoped-tls`, `siphasher`, `type-map`, `version_check`, `winapi`, `winapi-i686-pc-windows-gnu`, `winapi-x86_64-pc-windows-gnu`

**Unicode-3.0** (18): `icu_collections`, `icu_locale_core`, `icu_normalizer`, `icu_normalizer_data`, `icu_properties`, `icu_properties_data`, `icu_provider`, `litemap`, `potential_utf`, `tinystr`, `writeable`, `yoke`, `yoke-derive`, `zerofrom`, `zerofrom-derive`, `zerotrie`, `zerovec`, `zerovec-derive`

**Apache-2.0** (16): `ab_glyph`, `ab_glyph_rasterizer`, `accesskit_winit`, `codespan-reporting`, `gethostname`, `gl_generator`, `glutin`, `glutin_egl_sys`, `glutin_glx_sys`, `glutin_wgl_sys`, `khronos_api`, `owned_ttf_parser`, `rspirv`, `spirv`, `unicode-general-category`, `winit`

**Zlib OR Apache-2.0 OR MIT** (10): `bytemuck`, `bytemuck_derive`, `dispatch2`, `objc2-app-kit`, `objc2-core-foundation`, `objc2-core-graphics`, `objc2-io-surface`, `objc2-metal`, `objc2-quartz-core`, `objc2-ui-kit`

**MPL-2.0 OR GPL-3.0-only** (9): `librashader`, `librashader-cache`, `librashader-common`, `librashader-pack`, `librashader-preprocess`, `librashader-presets`, `librashader-reflect`, `librashader-runtime`, `librashader-runtime-wgpu`

**MIT OR Apache-2.0 OR Zlib** (6): `cursor-icon`, `glow`, `raw-window-handle`, `xkeysym`, `zune-core`, `zune-jpeg`

**Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT** (5): `linux-raw-sys`, `rustix`, `wasi`, `wasip2`, `wit-bindgen`

**Unlicense OR MIT** (5): `aho-corasick`, `byteorder-lite`, `memchr`, `termcolor`, `winapi-util`

**Apache-2.0/MIT** (4): `bytecount`, `halfbrown`, `pollster`, `rustc-hash`

**BSD-3-Clause** (3): `subtle`, `tiny-skia`, `tiny-skia-path`

**ISC** (3): `libloading`, `rustls-webpki`, `untrusted`

**Zlib** (3): `foldhash`, `slotmap`, `zlib-rs`

**BSD-2-Clause OR Apache-2.0 OR MIT** (2): `zerocopy`, `zerocopy-derive`

**BSD-3-Clause OR Apache-2.0** (2): `moxcms`, `pxfm`

**BSD-3-Clause OR MIT OR Apache-2.0** (2): `num_enum`, `num_enum_derive`

**BSL-1.0** (2): `clipboard-win`, `error-code`

**MPL-2.0** (2): `option-ext`, `persy`

**Unlicense/MIT** (2): `same-file`, `walkdir`

**(Apache-2.0 OR MIT) AND BSD-3-Clause** (1): `encoding_rs`

**(MIT OR Apache-2.0) AND OFL-1.1 AND Ubuntu-font-1.0** (1): `epaint_default_fonts`

**(MIT OR Apache-2.0) AND Unicode-3.0** (1): `unicode-ident`

**0BSD OR MIT OR Apache-2.0** (1): `adler2`

**Apache-2.0 AND ISC** (1): `ring`

**Apache-2.0 AND MIT** (1): `dpi`

**Apache-2.0 OR GPL-2.0-only** (1): `self_cell`

**Apache-2.0 OR ISC OR MIT** (1): `rustls`

**BSD-2-Clause** (1): `arrayref`

**CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception** (1): `blake3`

**CC0-1.0 OR MIT-0 OR Apache-2.0** (1): `constant_time_eq`

**CDLA-Permissive-2.0** (1): `webpki-roots`

**MIT / Apache-2.0** (1): `cgl`

**MIT OR Apache-2.0 OR LGPL-2.1-or-later** (1): `r-efi`

**MIT OR Zlib OR Apache-2.0** (1): `miniz_oxide`

**MPL-2.0+** (1): `smartstring`

