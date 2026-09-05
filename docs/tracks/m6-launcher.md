# Track: M6 — the launcher (doc 07)

The handoff for a session that works on the companion launcher: the
machine library grid, guided machine creation, snapshots UI, disc-shelf
editing, and packaging. Read `docs/00-status.md` first for the global
picture and the track rules, then doc 07 (the design: player vs.
launcher split, settings taxonomy, platform packaging) and doc 08's M6
section (scope, exit criterion). Branch: `track/m6-launcher` (opened
2026-09-04 off `main`), worktree `.claude/worktrees/m6-launcher`.

## Decided this session

- **UI toolkit: egui/eframe**, not Slint (doc 07 left this open;
  resolved 2026-09-04). Reasons: MIT/Apache-2.0 (no friction with the
  project's GPL-2.0-only + "everything open source" stance — Slint's
  non-GPLv3 tiers are royalty-free/commercial, not verified compatible),
  pure Rust, same toolkit doc 07 already names for the player's overlay.
  `eframe`'s default features are wgpu-backed already (no `glow`
  dependency pulled in) and its `wgpu`/`winit` versions (30.0.1 /
  0.30.13) unify with the ones `player/Cargo.toml` pins — one copy of
  each in `Cargo.lock`, not two. Keep it that way: if `player/`'s wgpu or
  winit pin ever moves, bump `launcher/Cargo.toml`'s `eframe`/`egui` pin
  in the same commit and rebuild both, or the workspace will carry two
  copies of a heavy dependency.
- `eframe::App` in 0.36 is `fn ui(&mut self, ui: &mut egui::Ui, frame: &mut
  eframe::Frame)`, not the older `update(ctx, frame)` — wrap the body in
  `egui::CentralPanel::default().show(ui, |ui| { … })` for margins/background
  (`show_inside` is deprecated in favor of the same-named `show`).

## Scope and files (this track owns them)

- `launcher/` entirely: `Cargo.toml`, `src/` (currently one `main.rs`
  skeleton; expect it to grow into `app.rs`, `library.rs` (machine
  bundle scanning + grid state), `wizard.rs` (guided creation),
  `bundle.rs` (the `machine.toml` format, shared conceptually with the
  player but not necessarily a shared crate yet — decide when the
  player needs to read the same format), `snapshots.rs`, `discshelf.rs`.
- Docs: doc 07 (this track's design doc — update as decisions land, e.g.
  the toolkit choice above), the M6 section of doc 08, this file, the M6
  row of the state table and "Next steps" in `docs/00-status.md`.
- Shared (rebase first, edit minimally, say which track in the commit):
  `Cargo.toml` (workspace members — already lists `launcher`), `CLAUDE.md`
  if a launcher-specific test tool is added to the table, `docs/00-status.md`
  outside the M6 row. The machine bundle format (`machine.toml`) will
  eventually be read by both `player/` and `launcher/`; when that lands,
  decide then whether it needs a shared crate — don't preempt it now.
- **`shader-chain/` is now a shared crate** (2026-09-05, factored out of
  `player/src/shader.rs` for the shader-preview feature below): both
  `player/` and `launcher/` depend on it for the librashader filter
  chain itself. A change here affects both binaries — rebuild and
  retest both (the player's `PLAYER_DUMP_OUT` dump-diff and the
  launcher's `--preview-shader` debug verb, both below) before pushing.

## State (2026-09-04)

- **Skeleton landed:** `launcher/Cargo.toml` takes `eframe`/`egui`
  0.36.1; `main.rs` opens a native window (`eframe::run_native`). This is
  the M0 stub replaced with a real window. Verified: `cargo build` clean
  (no warnings), the binary runs against the box's Wayland session for
  5 s with no crash (RADV's usual non-conformant-ICD notice only).
- **Step 1 (bundle format) landed:** `launcher/src/bundle.rs` — `Machine`
  (name, family, `ram_mb`, `disk`, `discs` shelf, `shader` override),
  serde + `toml` round-trip (`load`/`save`), `Machine::reference(family,
  name, disk)` from doc 06's defaults (256 MB Win98 / 512 MB XP), and
  `qemu_args(pc_bios_dir)` translating a bundle to the real
  `qemu-system-i386` command line the player expects (doc 06's per-family
  tables: `-vga cirrus` + `pcnet` + `sb16` for Win98, `-vga none -device
  d3dpt-vga` + `rtl8139` + `AC97` for XP; `-cpu pentium3` per doc 06's
  floor rationale; `usb-tablet`; the first disc shelf entry as a
  `bus=ide.1` CD-ROM, `audiodev=embed0` throughout — the id the player
  itself adds, per README). Verified by hand: bootstrapped both families
  against real `~/vms/*.qcow2` paths, round-tripped through real TOML
  files on disk, `--print-args` output checked field-by-field against
  doc 06 and the invocation shapes already validated elsewhere (README's
  player example, `tools/xp-cdimage-test.sh`'s `ide-cd` line); also
  checked with a disc-shelf entry added by hand. Not yet run through an
  actual QEMU boot (no `build/qemu` in this worktree) — that's the
  natural point to add a real check once step 3 (spawning a player)
  exists; see the testing note below.
- **Step 2 (library grid) landed:** `launcher/src/library.rs` —
  `default_dir()` (the platform data dir via the `directories` crate,
  `~/.local/share/win98-xp-virt/machines` on Linux; `LAUNCHER_LIBRARY_DIR`
  overrides it, matching the project's `PLAYER_*` env-knob convention),
  `slug()` (a directory name from a machine name, deduplicated against
  what's already there — "XP test box" twice became `xp-test-box` and
  `xp-test-box-2`), `create()` (makes the subdirectory, writes
  `machine.toml` from `Machine::reference`), `scan()` (one level of
  subdirectories, each needing a readable `machine.toml`; a corrupt one
  is skipped with a `[library]` stderr line, not fatal). `launcher --new`
  now writes into the library by default instead of an explicit output
  path (the same call the wizard, step 4, will eventually make);
  `main.rs` scans the library at startup and renders a real `egui::Grid`
  (name / family / bundle directory) in place of the empty state when
  non-empty. Verified visually: three bundles created via `--new`,
  screenshotted the real window over the Wayland session (`grim`) — the
  grid renders all three, striped, in a-z order, dedup slug correct.
  Running-state and thumbnails are not modeled yet (need step 3's process
  spawning; adding a fake/always-stopped field now would be the kind of
  half-built abstraction CLAUDE.md warns against).
- **Step 3 (spawn a player) landed:** `launcher/src/player.rs` —
  `player_binary()` (alongside the launcher's own executable via
  `current_exe()`; `LAUNCHER_PLAYER_BIN` overrides), `pc_bios_dir()`
  (`qemu/pc-bios`; `LAUNCHER_PC_BIOS_DIR` overrides — bundling it is a
  packaging, step 6, concern), `spawn(&Machine)` (builds `qemu_args`,
  `Command::new(player_binary()).arg("--").args(args).spawn()`).
  `LauncherApp` gained `running: HashMap<PathBuf, Child>` keyed by
  bundle dir; each grid row shows a "Play" button or a "Running" label,
  and `ui()` calls `ctx().request_repaint_after(500ms)` and
  `try_wait()`s every running child so exited ones drop out of the map
  without needing a click. **No stop/kill control was added** —
  CLAUDE.md: a killed VM leaves a dirty FAT, only a guest-side or
  player-window shutdown should end a run; the launcher only observes.
  Rust's `Child::drop` neither waits nor kills, which is exactly the
  independence a spawned player needs.

  Verified end to end for real, not just compiled: built `player` in
  this worktree by pointing `QEMU_EMBED_LIB_DIR` at the **main
  checkout's** already-built `build/qemu` (no fresh QEMU build needed —
  the embed API version matched, both checkouts at 6); created a
  throwaway empty qcow2 with `qemu-img`; added a `--play <machine.toml>`
  debug verb that calls the exact same `player::spawn` the "Play"
  button does. Ran it against a `win98` bundle pointed at the empty
  disk with `LAUNCHER_PC_BIOS_DIR` aimed at the main checkout's
  `qemu/pc-bios` (this worktree has no `qemu` submodule checked out —
  `git submodule status` shows it uninitialized here). The launcher
  process printed the child pid and exited immediately; the player
  process reparented to init (`ps` showed `PPID 1`) and kept running —
  confirmed the independence property, not just asserted it. Screenshot
  (`grim`) of the real window: SeaBIOS → "No bootable device" → the
  pcnet NIC's iPXE ROM starting and DHCP-configuring over the bundle's
  `-netdev user,id=n0 -device pcnet,netdev=n0` — proof the translated
  args are honored beyond just the disk line. Killed the throwaway
  process afterward (an empty test disk, no real guest, no dirty-FAT
  concern).

  **Caveat for the next session:** a plain workspace-wide `cargo build`
  from this worktree fails on `player` (`unable to find library
  -lqemu-embed-i386`) because there's no `build/qemu` here — this is
  pre-existing (CLAUDE.md's build order: prepare/configure/ninja before
  `cargo build`), not something this session broke. `cargo build -p
  launcher` (or `cd launcher && cargo build`) succeeds standalone since
  `launcher` only shells out to `player` at runtime, never links
  against `qemu-embed`. Testing `player.rs`'s spawn path again needs
  either `QEMU_EMBED_LIB_DIR=<main checkout>/build/qemu` (fast, reuses
  what's already built there) or a real `scripts/prepare-qemu.sh &&
  scripts/configure-qemu.sh && ninja` in this worktree.
- **Step 4 (guided creation wizard) landed:** `launcher/src/wizard.rs` —
  `Wizard` holds the form fields (family, name, existing-disk toggle +
  path or new-disk size in GB, optional install media, an "advanced"
  toggle + raw TOML text box). "New machine…" opens it
  (`egui::Window`); "Create" either builds a `Machine` from
  `Machine::reference` + the disc-shelf entry and (for a new disk) calls
  `player::create_disk` first, or — in advanced mode — validates the
  hand-edited TOML with `toml::from_str::<Machine>` *before* writing it
  (a bad edit must not silently corrupt the library with an unreadable
  bundle). Either way it goes through the new `library::reserve_dir`
  (factored out of `library::create`, which still backs `--new`): the
  bundle directory is made first so a new disk can be created inside it
  (`<dir>/disk.qcow2`) before the bundle that references it is written.
  On success `main.rs` rescans the library so the grid updates
  immediately, no restart needed.

  A `player::qemu_img_binary()` was added alongside `player_binary()`
  and `pc_bios_dir()` — `qemu-img` is a QEMU build product, not a
  workspace binary, so it's found the way test scripts already do
  (`build/qemu/qemu-img` relative to a checkout, `LAUNCHER_QEMU_IMG_BIN`
  overrides).

  Verified for real: a `--wizard-new <win98|xp> <name> <disk-size-gb>`
  debug verb calls the exact same `Wizard::create` the window's button
  does (`Wizard::with_new_disk` sets the same fields the widgets would).
  Ran it against `LAUNCHER_QEMU_IMG_BIN` pointed at the main checkout's
  `build/qemu/qemu-img`: created a real 4 GiB qcow2 *inside* the new
  bundle's own directory, wrote a `machine.toml` referencing it by
  absolute path — confirmed by reading both files back. The empty-name
  validation path was checked too (returns an `Err`, doesn't panic
  internally — the GUI's `Create` handler catches it into `self.error`).
  The resulting bundle showed up correctly in the real windowed grid
  (screenshotted with `grim`): name, family, directory, and a working
  "Play" button, exactly like a bundle made any other way — `scan()`
  doesn't care how a `machine.toml` got there. **Not verified by an
  actual click through the form** — no working mouse-click automation on
  this Wayland session (`xdotool` doesn't see native Wayland windows, no
  `ydotool`/`wlrctl` installed) — so the widget wiring itself (checkbox
  toggling which fields show, the advanced text box, the "Create" button
  reading current field values) is reviewed but not click-tested; a
  human should click through it once. The advanced-TOML branch reuses
  `toml::from_str::<Machine>`, already exercised elsewhere (`scan()`,
  `Machine::load`), so it wasn't re-tested in isolation.
- **Native file picker for the wizard's path fields** (user request,
  2026-09-04): egui draws pixels only, no OS dialogs, so
  `launcher/src/filepicker.rs` pairs a text field with a "Browse…"
  button that pops the real system dialog via `rfd` — NSOpenPanel /
  Win32 `IFileDialog` / the Linux XDG desktop portal (over D-Bus).
  `rfd = { default-features = false, features = ["xdg-portal"] }` keeps
  the Linux build off GTK entirely (mirrors how the user's own
  `~/work/nxvim` `bemtvi-gui` crate uses it — same crate, same
  rationale, checked directly before adding it here). `wizard.rs`'s
  `disk_path` (existing-disk case) and `install_media` fields now go
  through `filepicker::path_field` with extension filters (`qcow2/img/
  raw` for disks, `iso/cue/ccd/mds` for discs, matching the M5 CD-ROM
  formats); typing a path directly still works, the button is a
  convenience. The dialog call blocks the calling thread, which is fine
  here — this launcher has no async runtime for it to stall (unlike a
  tokio-based GUI, where the same blocking portal call has to stay off
  the runtime's own threads).

  Verified for real, not just compiled: `filepicker::pick_file_headless`
  (the same call `path_field`'s button makes) wired to a `--pick-file`
  debug verb, run standalone — a genuine GTK-backed portal file dialog
  opened over the Wayland session (`xdg-desktop-portal-gtk` was already
  running), screenshotted with `grim` showing a real directory listing.
  Couldn't cancel it via `wtype -k Escape` (the key didn't reach the
  dialog's surface — a Wayland virtual-keyboard-vs-focus quirk, not a
  bug in this code) so the process was killed directly; harmless, it's
  just a file-picker window with no state to corrupt. This proves the
  portal wiring itself works end to end; clicking through the wizard's
  "Browse…" button specifically is still unverified for the same
  click-automation reason as the rest of the wizard — one more thing for
  the human click-through pass.
- **Editing an existing machine** (user request, 2026-09-04): the same
  form now doubles as "Edit machine" instead of only creating new ones.
  `Wizard::open_edit(machine, bundle_path)` pre-fills the fields from an
  existing `Machine` and stashes an `EditTarget` — `ram_mb`, `shader`,
  and any disc-shelf entries beyond the first (this form only edits the
  single "install media" slot) — plus the bundle's exact original text,
  used verbatim as the advanced box's starting point instead of a
  reconstruction. `create()` was renamed `submit()` and now branches:
  editing writes back to the existing bundle path with `build_machine()`
  merging the edited fields onto the preserved ones, **never renaming
  the bundle directory** even when the display name changes (external
  references to the directory, and the disk path inside it, stay valid).
  A "New machine" still reserves a fresh directory exactly as before —
  `build_machine()` is the one place both paths agree on what a
  `Machine` looks like, so they can't silently diverge. The grid gained
  an "Edit…" button next to "Play"/"Running" (same cell, side by side).

  Verified for real, not just reviewed: hand-crafted a bundle with a
  non-default RAM value (768, not XP's 512 default), two disc-shelf
  entries and a `shader` override — fields the wizard's own UI doesn't
  expose at all — then renamed it via a `--wizard-edit <machine.toml>
  <new-name>` debug verb (the same `submit()` the "Save" button calls).
  Read the file back: name changed, **RAM/discs/shader all intact**,
  directory unchanged. `--print-args` on the result showed `-m 768`,
  confirming the preserved value reaches the real translated command
  line, not just the TOML. The empty-name validation fires in edit mode
  too. Screenshotted the real grid showing "Edit…" next to "Play" for an
  entry. **Not click-tested** for the same reason as the rest of the
  wizard — a human should click "Edit…", change a field, and click
  "Save" once.
- **Bugfix: "New machine" without a custom disk failed with "No such
  file or directory"** (user-reported, 2026-09-04). `player::pc_bios_dir`
  and `player::qemu_img_binary` defaulted to bare relative paths
  (`"qemu/pc-bios"`, `"build/qemu/qemu-img"`) resolved against the
  *process's current working directory* — fine only when the launcher
  happens to be started with that directory as the workspace root, not
  guaranteed for a real launch (double-click, a shortcut, `cargo run`
  from a subdirectory). `player_binary` was already immune to this
  (`current_exe()`-relative), but disk creation always goes through
  `qemu_img_binary`, so any "new disk" flow from the wrong cwd broke —
  the existing-disk flow never called it, which is why only "no custom
  image" reproduced. Fixed by anchoring both at the build-time
  `CARGO_MANIFEST_DIR` (baked into the binary via `concat!(env!(...))`),
  the same technique `qemu-embed/build.rs` already uses for its own
  default — not a new pattern for this codebase. `LAUNCHER_PC_BIOS_DIR`/
  `LAUNCHER_QEMU_IMG_BIN` still override it. Also improved `create_disk`
  and `spawn`'s error messages to name the resolved binary path on a
  spawn failure (previously a bare `Os { code: 2, .. }` with no context).

  Verified for real: reproduced the exact bug first (`cd /tmp &&
  launcher --wizard-new win98 "…" 2` → `Os { code: 2, kind: NotFound,
  message: "No such file or directory" }`, matching the report
  verbatim), confirmed the fix via `--print-args` from `/tmp` showing an
  absolute, cwd-independent `-L` path, then ran the full "new machine,
  new disk" flow from `/tmp` with `LAUNCHER_QEMU_IMG_BIN` pointed at the
  main checkout's real `qemu-img` (standing in for a real checkout that
  has `build/qemu` built, which this worktree still doesn't) — created a
  real qcow2 and a correct bundle. This worktree's own *default* (no
  override) still fails the same way, but that's the pre-existing,
  already-documented "no `build/qemu` here" limitation, not the cwd bug.
- **Bugfix: "Browse…" should open where the field already points** (user
  request, 2026-09-05). `filepicker::pick_file_headless` gained a
  `start_dir: Option<&Path>` parameter (`rfd::FileDialog::set_directory`);
  a new `filepicker::start_dir(value)` extracts it from a path field's
  current text — the value's own directory if it names one (a file
  inside it, or the directory itself), `None` (the OS default) if the
  field is empty or names a bare filename. `path_field`'s "Browse…" now
  passes `start_dir(value)`, so re-opening it browses from wherever the
  field already points instead of always the platform default (home).

  Verified for real, including a self-caught bug in the verification
  tool itself: first tried exercising this by passing a path straight
  through `--pick-file`'s new optional arg to `set_directory` — with a
  *directory* argument it worked (screenshotted opening in it), but with
  a *file* path (what the wizard's fields actually hold) the dialog
  opened to a broken empty view (confirmed by screenshot: no breadcrumb,
  no listing) — proving `set_directory` needs a directory, not a file,
  which is exactly why `path_field` needs its own parent-extracting
  `start_dir()` rather than passing the raw field value. Fixed the debug
  verb to call the same `filepicker::start_dir()` `path_field` uses
  (made `pub` for this) instead of a separate stand-in, then reverified:
  a file path now correctly opens the dialog in its parent directory
  (screenshotted), and no argument still falls back to the OS default
  (also screenshotted, unchanged from before this fix).

- **Shader profile manager** (user request, 2026-09-05): named, reusable
  shader presets + parameter overrides, independent of any one machine.
  `launcher/src/shader_profile.rs` (`ShaderProfile`: name, `.slangp`
  path, a sparse `params: BTreeMap<String, f32>` — only overrides are
  stored, so a profile survives the preset gaining new parameters) and
  `shader_library.rs` (flat `<slug>.toml` files under a new
  `shader-profiles` platform-data-dir library, `LAUNCHER_SHADER_PROFILES_DIR`
  override — mirrors `library.rs`'s scan/create/slug shape, one file per
  profile instead of a bundle subdirectory). `shader_manager.rs` is the
  manager window: a New/Edit/Delete list, and an editor that parses the
  chosen preset via `librashader::presets::{ShaderPreset,
  get_parameter_meta}` (a new introspection-only `librashader` dep in
  `launcher/Cargo.toml`, `presets`+`preprocess` features, no runtime
  backend) and draws a checkbox+slider per declared parameter (min/max/
  step/description straight from the shader source's `#pragma
  parameter`). `bundle::Machine` gained `shader_profile: Option<String>`
  (a profile id, takes precedence) alongside the existing raw `shader`
  override (now the advanced/hand-written-bundle escape hatch); the
  wizard gained a "Shader profile" combo box for new and edited
  machines. `player.rs::resolve_shader`/`shader_args` resolve a
  machine's profile into `player`'s own `--shader`/`--shader-params` at
  spawn time.

  Player side: `player/src/shader.rs::Chain::load` takes `params: &[(String,
  f32)]`, applied via `librashader::runtime::FilterChainParameters`
  (`RuntimeParameters::update_parameters`) after the chain loads; an
  unknown parameter name is skipped with a stderr line rather than
  failing the machine. `main.rs` parses a new `--shader-params
  <name=value,...>` flag / `PLAYER_SHADER_PARAMS` env var, comma-separated
  like `PLAYER_KEYS`; documented in README.md and `shaders/README.md`.

  Verified for real: the launcher pipeline end to end through new debug
  verbs (`--new-shader-profile`, `--set-shader-param`,
  `--list-shader-params`, `--assign-shader`, `--print-shader-args`) —
  listed the real 13 parameters of
  `third_party/slang-shaders/crt/crt-lottes.slangp`, created a profile,
  overrode `brightBoost`, assigned it to a machine, and
  `--print-shader-args` resolved to the exact `--shader … --shader-params
  brightBoost=1.8` `spawn()` would pass. Player side, built via the same
  sibling-worktree `QEMU_EMBED_LIB_DIR` trick as step 3: dumped the test
  pattern with `PLAYER_DUMP_OUT` with and without the override — the two
  PNGs differ from the first bytes, proving the override reaches the
  actual rendered pixels; a bogus parameter name alongside a real one
  warned and didn't crash, and the real one still applied.
  `cargo build --workspace` clean. **Not click-tested** — same Wayland
  gap as the rest of this track — a human should click through "Shader
  profiles…" and the wizard's new combo box once.

- **Live shader preview** (user request, 2026-09-05): the profile editor
  gained a second column that runs a chosen image through the real
  filter chain and shows the result, re-rendering as sliders move.
  Because a CRT preset's scanline/mask math depends on the actual pixel
  size it renders at (egui stretching a small render up afterward would
  just blur it away), the chain-running code — previously
  `player/src/shader.rs`'s `Chain` — moved into a new shared workspace
  crate, `shader-chain/`, so the player and the launcher run librashader
  the same way instead of two implementations drifting apart.
  `Chain::load`/`set_parameters` split (load once per preset, re-apply
  parameters on every slider tick without recompiling shaders — a
  reload would be far too slow for a live preview); `player/src/main.rs`
  updated to match (`mod shader` → `use shader_chain as shader`,
  otherwise unchanged behavior).

  `launcher/src/shader_preview.rs`'s `Preview` runs on the
  `wgpu::Device`/`Queue` eframe already opened for egui
  (`egui_wgpu::RenderState`, via `eframe::wgpu`/`eframe::egui_wgpu` — no
  separate `wgpu` pin in `launcher/Cargo.toml`, Cargo unifies it with
  `shader-chain`'s) rather than a second GPU context: decodes the chosen
  image (new `image` crate dep, `png`/`jpeg`/`bmp` features) into an
  `Rgba8Unorm` input texture, runs the chain at a scale of the image
  clamped to fit a ~480×360 pane (shrinking a large screenshot,
  upscaling a small one so the mask is visible at all — same reasoning
  as the player rendering its chain at viewport size, not the guest's
  native resolution), and registers the output as an egui texture via
  `egui_wgpu::Renderer::register_native_texture`/
  `update_egui_texture_from_wgpu_texture` (same `TextureId` reused
  across reruns, freed on `Drop`). `LauncherApp` captures
  `cc.wgpu_render_state.clone()` once at startup; `None` (a non-wgpu
  backend, not expected given the toolkit decision) shows "no live
  preview" instead of panicking.

  Verified for real: a new `--preview-shader <preset> <image> <out.png>
  [name=value,...]` debug verb builds a real windowless
  `egui_wgpu::RenderState` (the same `RenderState::create` call eframe
  itself makes) and drives `Preview` exactly as the editor's preview
  column does, dumping the frame via a new `shader_chain::Chain::
  output_texture()` accessor. Ran it against a real 2560×1920 PNG (shrank
  correctly to 480×360) and a real 64×64 RGBA icon (`qemu/ui/icons/
  qemu_64x64.png`, exercising the alpha-channel decode path — upscaled
  correctly to 360×360, `min(480/64, 360/64) = 5.625×`); with and
  without `brightBoost=1.8` produced dumps differing from the first
  byte, same proof-of-liveness as the shader-profile-manager work, and
  visually confirmed (read back as images) the icon shows real
  scanline/mask texture from `crt-lottes.slangp`, not a pass-through
  copy. `cargo build --workspace` clean, no warnings, after the full
  `shader.rs` → `shader-chain/` move. **Not click-tested through the
  actual editor window** — same Wayland gap as the rest of this track.

- **Bug fixed (user-reported, 2026-09-05): preview showed a solid black
  image-sized shape.** First investigated without reproducing it (the
  `--diag-preview-frame`/`LAUNCHER_DEBUG_SHADER_PREVIEW` debug tools
  below were built for this) against a few CRT presets and small
  game-resolution images — all correct. The user then gave the exact
  repro: `crt-aperture.slangp` against a real photo (1025×791, from
  `~/Pictures`). Reading `crt-aperture.slang`
  (`third_party/slang-shaders/crt/shaders/crt-aperture.slang:145-148`)
  found the actual bug: `scale = floor(OutputSize.y / SourceSize.y)`,
  then `offset = 1.0 / scale * 0.5` — a **divide by zero** the moment the
  render target is smaller than the source, i.e. the moment the shader
  is asked to *shrink* rather than upscale. The preview's own scaling
  (`shader_preview.rs`) was shrinking any image bigger than the ~480×360
  pane to fit it, which a 1025×791 photo triggers and a small game
  screenshot never does — exactly why every case tried first (small
  icons, an upscaled test pattern) worked and the user's real photo
  didn't. This isn't really this one preset's bug: RetroArch/libretro
  slang CRT presets are written on the assumption that they upscale a
  small *native* resolution, the same assumption the player's own doc 03
  pipeline makes (guest-native input, viewport-sized — always ≥ — output).

  Fixed on our side, not upstream (`third_party/slang-shaders` is a
  submodule): `load_image` now downsizes an oversized source *on the
  CPU* (`image::DynamicImage::resize`, `FilterType::Triangle`) to fit the
  pane **before** the shader ever sees it, so the shader is only ever
  asked to upscale; `render`'s scale computation is now `clamp(1.0, 8.0)`
  (was `clamp(0.1, 8.0)`) to match — both by construction (post-resize,
  the source can't exceed the pane) and as a defensive floor.

  Verified for real: `--preview-shader` against the user's exact
  preset+photo now renders the actual (correctly CRT-shaded) image
  instead of black; re-ran every previously-working case (small icon and
  a bigger icon through `crt-lottes.slangp`, the small icon through
  `crt-aperture.slangp` itself, the photo through `crt-royale.slangp`
  too) to confirm no regression — all still correct. The photo is a
  personal document image (the user's driver's license) from
  `~/Pictures`; none of the render outputs were kept (`rm`'d after
  visual/size-based verification, never committed).

  **Debug tooling from the investigation, kept:** `--diag-preview-frame
  <preset> <image> <out.png>` renders one full egui frame the way
  eframe's own paint step would (tessellate → `update_texture`/
  `update_buffers` → a real render pass) and dumps the composite —
  useful for any future "the preview looks wrong" report to rule the
  egui-compositing layer in or out. `LAUNCHER_DEBUG_SHADER_PREVIEW=
  <preset>;<image>[;fullscreen]` opens the editor pre-filled at startup
  (optionally with the fullscreen toggle below already on), for
  screenshotting the *real* windowed app without a GUI click.

- **Preview reworked to match the player exactly** (user request,
  2026-09-05, same day): "I want to pick a 640x480 image and see exactly
  how it will look in the player, integer scaling and all" plus a
  fullscreen toggle that gives the sliders the horizontal space the
  letterboxed image doesn't need. `shader_preview.rs::Preview::render`
  now runs the *exact* formula `player::Gpu::viewport` uses — `scale =
  (area.x/iw).min(area.y/ih).floor().max(1.0)` — instead of the ad hoc
  "fit inside a small fixed pane" scale from the first preview cut, and
  `shader_manager.rs::preview_ui` paints the result centered in a
  black-filled area via `ui.painter_at(rect).image(...)` (an
  `egui::Image` widget would stretch to whatever size it's given,
  losing the "always an *integer* multiple" property entirely) instead
  of an image widget sized to fill its slot. Cropped-at-the-edges when
  the native resolution doesn't fit the area — same as making the real
  player's window smaller than the guest's resolution.

  `render`'s own `.max(1.0)` now guarantees no downscale request ever
  reaches the shader regardless of the source image's size, which is
  actually *why* the previous bugfix's CPU pre-resize (still in
  `load_image`, `MAX_SOURCE_W`/`H`) is no longer load-bearing for
  correctness — it's now just a sanity cap (1600×1200) against treating
  an arbitrarily huge photo as "native resolution" and rendering it at
  full size every frame, not what stands between the user and another
  black-frame divide-by-zero.

  Layout (the window-sizing half of this was reworked the same day, see
  the vertical-resize bugfix below — `max_size(900×700)` and the
  480×360 preview floor described here are gone): a fixed-width (300px)
  controls column + the rest of the
  window for the preview (`editor_ui`, replacing the old 50/50
  `ui.columns`) — growing the window (the new "Fullscreen" checkbox
  next to "Preview", `Editor::fullscreen`) grows the *preview*, not the
  sliders, matching the user's ask to use the freed-up width for
  controls rather than wasted black bars. Fullscreen forces
  `egui::Window::fixed_rect(ctx.viewport_rect())`; turning it back off
  falls back to `.max_size(900×700)`, since egui otherwise remembers a
  window's last (now huge) rect across frames and `default_width` only
  applies the very first time a window is ever shown. The non-fullscreen
  case also floors the preview area at 480×360 — otherwise an
  auto-sizing egui window shrinks its content to whatever's left over
  rather than growing to fit a request, so without a floor the compact
  window would squeeze the preview down to a sliver and crop most of a
  640×480 image out of view.

  Verified for real on the actual windowed app (`LAUNCHER_DEBUG_SHADER_
  PREVIEW`, screenshotted): a synthetic 640×480 test image through
  `crt-lottes.slangp` renders at native 1:1 in the compact window (no
  visible scanlines at scale 1 — correct: there's no sub-pixel gap
  between rows to darken until you're actually upscaling, exactly what
  the real player would also show for a window barely bigger than the
  guest's own resolution) and at a visibly higher integer scale,
  properly letterboxed with black bars either side and the sliders
  filling that freed width, once "Fullscreen" is on. Re-ran the
  crt-aperture/large-photo combination from the bugfix above through the
  new algorithm with no CPU pre-resize needed to protect it (a 480×360
  area against a 1025×791 photo: `floor(min(480/1025,360/791)) = 0`,
  `.max(1.0) = 1` — native resolution, letterboxed/cropped, not
  black) — confirms the `.max(1.0)` alone is sufficient, independent of
  the sanity cap. `cargo build --workspace` clean, no warnings.

- **Bugfix — the editor window only resized horizontally** (user
  report, 2026-09-05): "the window only resizes horizontally. It starts
  very short and after picking an image it grows but the dials for the
  shader stay with the original short height." All three symptoms are
  one root cause: an `egui::Window` takes the size of its *content*, and
  `editor_ui` was a plain top-to-bottom stack of auto-sized widgets, so
  dragging the bottom edge grew `Resize`'s remembered `desired_size`
  while the window still drew itself at content height and snapped back.
  Width worked by accident: the preview pane asked for
  `available.x.max(480)`, so a wider window really did produce wider
  content. The same accident explains "starts very short" (with no
  preview image the preview column returns early and asks for nothing)
  and "the dials stay short" (the two columns were independent
  `ui.vertical`s inside a `ui.horizontal` — the slider column sized
  itself to its own content, the preview column set the row's height).

  Fixed by giving the editor a fill-the-window layout instead of a
  stack: `egui::Panel::top` for the name + preset header,
  `egui::Panel::bottom` for the error + Save/Cancel footer (both
  `Frame::NONE`, `show_separator_line(false)`, keeping the hand-drawn
  `ui.separator()`s), `CentralPanel` for the body. Panels expand to the
  parent `Ui`'s `max_rect`, which *is* `desired_size`, so the content
  now always matches the window, both columns get the same height from
  `ui.available_height()`, the params `ScrollArea` takes
  `auto_shrink([false, false])`, and the preview takes exactly what's
  left (the 480×360 floor is deleted — the window's own `min_size`
  560×360 is what keeps it usable, and asking for more than exists
  would just push the window bigger every frame). The window gets
  `default_size(980×700)` rather than `default_width(760)`, and the
  profile *list* screen now uses a separate `egui::Window::id`
  (`shader-list` vs `shader-editor`) so the editor's remembered size
  doesn't drag the two-row list open to full width. `max_size(900×700)`
  is gone — it had been the thing capping how tall the window could
  ever be dragged; un-fullscreening now restores the pre-fullscreen
  rect, which `ShaderManager` remembers itself (`windowed_rect` every
  non-fullscreen frame, applied once as `fixed_rect` on the frame after
  the toggle goes off; egui re-snaps the *position* to where fullscreen
  left it, the size sticks).

  **Second bug, found while verifying the first:** the first frame of a
  freshly opened preset silently marked `warpX`/`warpY` as overridden
  (0.031 → 0.030). A greyed-out `egui::Slider` still snaps its value to
  `step_by` and reports `changed()`, and several presets ship defaults
  off their own step grid — so `*over = Some(value)` fired for a
  parameter nobody had touched and "Save" wrote it into the profile.
  The slider now only takes a step (and only accepts a change) while
  its override checkbox is actually ticked, so an un-overridden
  parameter also displays the preset's true default.

  **Verified headlessly on the real editor window**, since this session
  still has no GUI click automation: a new `--diag-editor-frame
  <preset.slangp> <image> <out.png> [<screen WxH>] [<drag dy>]
  [<x,y;x,y clicks>]` verb runs the actual `ShaderManager::show`
  through `egui::Context::run_ui` frame by frame with synthetic pointer
  events, prints the window rect per frame and dumps the composited
  final frame (`--diag-preview-frame`'s render-a-frame code is now
  shared with it as `dump_egui_frame`/`apply_texture_deltas`/
  `headless_render_state`; `preset` = `list` shows the profile list
  instead of the editor). Results: a 150 px drag of the bottom edge
  takes the window 980×700 → 980×850 and it stays there after the
  release (before the fix the height was pinned by content); the dumps
  show the slider column and the preview both filling the taller
  window; a synthetic click on "Fullscreen" fills the 1400×900 screen
  and a second click returns to 980×850; with no image picked the
  window still opens at 980×700 (the "starts very short" complaint) and
  `warpX`/`warpY` read 0.031/0.041, unticked. The list screen is a
  compact 560×92. `cargo build --workspace` clean, no warnings. **Still
  not click-tested by a human.**

- **Step 5a — disc-shelf editing landed** (2026-09-05):
  `launcher/src/discshelf.rs` is a per-machine "Discs (n)…" window off the
  library grid that edits `Machine::discs` — the ordered shelf whose
  *first* entry `qemu_args` attaches as the boot CD-ROM. Add (the
  `filepicker` field, same `iso/cue/ccd/mds` filter as the wizard's
  install-media slot), Up/Down, Remove, and doc 07's **one-click
  guest-tools ISO attach**: `discshelf::guest_tools_iso()` finds the
  newest `guest-tools/out/guest-tools-*.iso` (the name `scripts/test.sh`
  already globs), canonicalized because unlike `pc_bios_dir` this path
  gets written *into* a bundle file, and `LAUNCHER_GUEST_TOOLS_ISO`
  overrides it; the button is greyed with a reason when none is built.
  `save()` re-reads the bundle and replaces only `discs`, so nothing
  else in the file can be lost (and a wizard save in between isn't
  clobbered). The window stays usable while the machine runs — nothing
  it writes touches a live guest — and says so instead of greying out;
  live media change is 5c below.

  This is deliberately the half of step 5 that needs **no IPC at all**:
  it's a bundle edit, so it applies at the next boot.

  Verified for real, and this time *through the actual widgets*: a new
  `--diag-shelf-frame <machine.toml> <out.png> [WxH] [x,y;x,y] [running]`
  verb runs the real `DiscShelf::show` through `egui::Context::run_ui`
  with synthetic pointer clicks and dumps the composited frame (the
  click/paint machinery factored out of `--diag-editor-frame` as
  `diag_window_frames`/`parse_clicks`). Clicking "Down" on row 1 then
  "Save" reordered the shelf and wrote it; "Remove" on row 3 dropped that
  disc; "Add guest-tools ISO" appended the found ISO and "Save" wrote a
  four-disc shelf — each confirmed by reading `machine.toml` back, with
  `ram_mb = 768`, `shader_profile` and `shader` (fields this window
  doesn't model) intact every time, and `--print-args` attaching the
  new first entry as `ide-cd`. Newest-ISO selection was checked against
  three files with staggered mtimes (it correctly ignores a newer
  `d3dpt-driver.iso`, not a guest-tools build), and the greyed-out
  no-ISO state was screenshotted too. Two layout bugs were found and
  fixed *by* those dumps: long disc paths widened the grid until the
  buttons sat off-screen (buttons moved before the path, path split
  file-name + truncating directory, window `max_width`), and `↑`/`↓`
  rendered as tofu in egui's default font (now "Up"/"Down").
  A headless `--disc-shelf <machine.toml> [<disc>|+tools ...]` verb does
  the same edit without a window, for scripting.

  Still **not click-tested by a human**; the library grid's own new
  "Discs (n)…" button is the one part not covered headlessly (the grid
  lives inside `eframe::App::ui`, which needs a real window).

- **Step 5b — snapshots (offline) landed** (2026-09-05):
  `launcher/src/snapshots.rs` — a per-machine "Snapshots…" window off the
  library grid listing the qcow2's internal snapshots (name, when, VM
  state size), with Take / Restore / Delete. A machine that isn't running
  has no monitor to ask, so this goes at the image with `qemu-img`, which
  is exactly what `savevm`/`loadvm` write into; live snapshots are 5c.
  Listing is `qemu-img info --output=json`, not `snapshot -l`: the JSON
  is a stable interface and the table is formatted for humans with no
  escaping for a tag containing a space (which the UI happily produces).
  `qemu-img`'s own stderr becomes the window's error text — "Could not
  find snapshot 'x'" says more than an exit code.

  Two safety rules are baked in. Every operation is refused while the
  machine is running, with a note saying to shut the guest down:
  `qemu-img` writing to an image QEMU has open corrupts it, and even the
  listing wants an image lock QEMU already holds. And "Restore" arms a
  second button ("Discard current state?") before it runs — rolling the
  disk back has no undo, and it sits one row away from "Delete".

  Verified against real QEMU output, through the real widgets: a new
  `--diag-snapshots-frame` (same shape as `--diag-shelf-frame`) plus a
  headless `--snapshots <machine.toml> [take|delete|restore <name>]`.
  Against a real 256 MB qcow2 made with the checkout's own `qemu-img`:
  took two snapshots with spaces in their names and listed them; a
  widget-driven click on "Restore" armed the confirmation on *that* row
  only (screenshotted) and the second click ran it, leaving `restored
  "after drivers"` in the status line; a widget-driven "Delete" dropped
  a row; and — this needed `diag_window_frames` to learn to type, a
  `+text` step in its input script — clicking the "New snapshot" field,
  typing a name and clicking "Take snapshot" created snapshot 3. Then a
  **real `savevm`** (a live `qemu-system-i386` driven over QMP
  `human-monitor-command`) against the same image: the window lists it
  with `1.2 MB` of VM state, where the `qemu-img`-made ones correctly
  show `—`. Error paths checked too: deleting a snapshot that isn't
  there, and a bundle pointing at a disk that doesn't exist, both
  surface `qemu-img`'s message.

- **Step 5c — live control landed** (2026-09-05), and with it **step 5 is
  done**. `launcher/src/control.rs`: the launcher adds
  `-qmp unix:<runtime dir>/<bundle>-<hash>.qmp,server,nowait` to the
  arguments it spawns the player with and speaks QMP to that socket
  itself. **No new protocol, no player change, no IPC surface on either
  binary** — QEMU allows several monitors, so the player's own
  in-process one (`player/src/qmp.rs`, on a socketpair with no
  filesystem path) is untouched, and this is the same shape
  `tools/qmpc.py` already uses to drive a guest. A bundle run straight
  through `player` by hand simply has no launcher socket, which is doc
  07's "the launcher is optional" path. The socket path is derived from
  the bundle directory, so any window can find it again without the app
  carrying it around; the directory is forced to 0700 (a QMP monitor is
  complete control of the machine) and a stale socket left by a *killed*
  player is removed before spawn, since QEMU refuses to bind over one.

  What it drives:
  - **Disc shelf:** each row gets "Insert" while the machine runs, plus
    an "Eject" — `blockdev-change-medium` / `eject`, which do
    open/eject/insert/close as one command, what a guest expects of a
    disc swap. No `format` argument, so a `.cue`/`.ccd` still probes to
    the `cdimage` driver (doc 17) exactly as on the command line.
  - **Snapshots:** the same window, now listing from
    `query-named-block-nodes` (whose `image.snapshots` is the same shape
    `qemu-img info --output=json` returns, so one kind of row either
    way) and running `snapshot-save`/`-load`/`-delete`. Those are QMP
    *jobs*, so the window starts one and polls `query-jobs` on its
    repaint tick rather than blocking the UI thread while QEMU writes a
    guest's RAM; buttons grey out while a job is in flight. A restore
    stops the VM first (QEMU requires it) and resumes it after **only if
    it was running** — a machine the user had paused stays paused.

  Two bundle-format consequences, both deliberate: `qemu_args` now gives
  the CD-ROM device an id (`control::CDROM_ID` = `ide1-cd0`, the same id
  `tools/xp-cdimage-test.sh` uses) so a medium change can name it, and
  **always attaches the drive, empty tray and all** — a drive that only
  existed when the bundle happened to ship a disc could never be loaded
  later, and a PC of the era has one regardless.

  Unix sockets only, so live control is Linux/macOS; on Windows the
  socket is never created and every live operation says so rather than
  the window pretending otherwise (a named pipe or a loopback port is a
  packaging-time, step 6, question). Snapshot *node names* are looked up
  at runtime rather than pinned in `qemu_args` — QEMU generates them
  (`#block136`) and putting one in the bundle format would freeze a
  command-line implementation detail.

  **Verified against real QEMU, then against the real player.** First a
  stand-in: `qemu-system-i386` on the *exact* `--print-args` command line
  plus the socket the launcher would have added. Through it, all of
  live listing (identical rows to the offline path), `take` (a real 2.8
  MB VM-state snapshot), `restore` and `delete`; then the same take
  driven entirely through the real window's widgets
  (`--diag-snapshots-frame` with a `+text` typing step: click the field,
  type, click "Take snapshot" → a 3.6 MB snapshot); live "Insert" both
  from `--insert-disc` and from a synthetic click on the row's button,
  each confirmed by `query-block` showing the medium actually changed,
  and "Eject" leaving `tray_open: true` on the `ide1-cd0` qdev. Then the
  **real `player` binary** (built here with `QEMU_EMBED_LIB_DIR` pointed
  at the main checkout's `build/qemu`): it took the extra `-qmp` without
  complaint, ran both monitors at once (`[qmp] connected: QEMU 9.2.4`
  from its own, ours serving the launcher), and a disc swap and a live
  snapshot both worked against it; a stale socket file planted
  beforehand was correctly replaced. Error paths: nothing running gives
  "No such file or directory" on the socket instead of a hang, and the
  offline path still lists every snapshot the live path made.

  Two bugs were found and fixed by this verification. `disk_node` matched
  a block node by filename alone — but a qcow2 shows up as *two* nodes,
  the qcow2 format node and the `file` protocol node under it, both
  reporting the same filename, and only the former can hold a snapshot;
  the match now requires `drv == "qcow2"`. And a failed live restore
  reported success: the post-operation `reload()` cleared `error` on its
  way to re-reading the list, so `Snapshot 'nope' does not exist` was
  wiped before anything showed it. `reload()` no longer touches `error`
  at all; only its callers decide. (Not a bug: `snapshot-delete` for a
  tag that isn't there concludes *without* an error — checked by hand
  against QEMU — because deleting a nonexistent internal snapshot is a
  no-op at the qcow2 level. A genuinely failing job does carry `error`,
  confirmed with a bad `snapshot-load`.)

- **The disc shelf became shared** (user request, 2026-09-05): "having
  one per machine doesn't make much sense". Right — a rip of Blood disc
  2 is a property of the person, not of the XP box that installed it
  first; two machines wanting the same disc had to list it twice, and a
  disc added while setting one machine up was invisible to the next.

  `launcher/src/disc_library.rs` is now the shelf: one flat `discs.toml`
  beside `machines/` and `shader-profiles/` in the platform data dir
  (`LAUNCHER_DISC_LIBRARY` overrides), entries being `{label, path}` —
  **labelled**, because a shelf of `d1.cue`, `disc2.cue`, `cd1.iso` is
  not a library; the label defaults to the file stem and is editable in
  place. All a machine keeps is `Machine::disc`, the single disc in its
  drive at boot; `Machine::discs` survives only to read pre-existing
  bundles (`boot_disc()` falls back to its first entry) and `save()`
  drops it, so a bundle migrates the first time anything writes it.
  Nothing is lost in between: `DiscLibrary::import_legacy` folds every
  legacy `discs` entry onto the shared shelf at startup, deduplicated by
  path so it can just run every time.

  `discshelf.rs` is one window with two modes. From the bottom button row
  ("Disc shelf…") it manages the collection — add, label, remove. From a
  machine's row ("Discs…") it shows the same shelf plus that machine's
  two disc decisions: a per-row **Boot** toggle (a bundle edit) and,
  while the machine runs, a per-row **Insert** (a monitor command). The
  shelf is deliberately *not* filtered per machine — any disc can go in
  any drive. Library edits save as they're made; there's no Save button,
  because a shelf is a list of things you own, not a document being
  drafted.

  Verified through the widgets again (`--diag-shelf-frame` now takes
  `shelf` in place of a bundle for the library-only mode): the migration
  moved four discs off a legacy bundle and was idempotent on a second
  run; a click on row 2's "Boot" wrote `disc = …/disc1.iso` to the
  bundle, highlighted that row only and updated both the header and the
  status line; `--print-args` attaches the chosen disc, and "Boot with an
  empty tray" leaves `if=none,id=cd0,media=cdrom` with no `file=`; a
  label typed into a row persisted to `discs.toml`; the bundle's
  `ram_mb`/`shader`/`shader_profile` survived every write. The headless
  verbs are now `--discs [add <disc>|+tools|remove <disc>]` and
  `--boot-disc <machine.toml> <disc|none>` (replacing `--disc-shelf`).
  One layout bug found by the dumps: a bare `TextEdit` in a grid cell
  claims almost no width, so the label column collapsed to five
  characters — `add_sized`, not `desired_width`.

- **The shelf, from inside the guest** (user request, 2026-09-05): "a
  program that works on both dos, win98 and xp, this program talks to the
  host and shows the list of cds on the shelf, then I can use the same
  program to insert one of the shelf cds in the tray directly from inside
  the machine". Only the host half is built so far; the guest program is
  the next step.

  **Transport, decided.** The program must run on DOS, Win98 and XP,
  which rules out most channels: XP blocks ring-3 port I/O, DOS has no
  networking worth the name, and the d3dpt device (doc 14) is XP-only.
  What all three *do* have is a way to send a raw command to their own
  optical drive — direct ATAPI PIO on DOS (exactly what
  `tools/atapi-guest-test.py` already does), ASPI on Win98, SPTI
  (`IOCTL_SCSI_PASS_THROUGH_DIRECT`) on XP — and we own that drive's
  firmware, because it is our ATAPI model (patch 51). So the shelf is a
  **vendor-specific ATAPI command** (opcode 0xD0, in MMC's vendor range)
  on the drive itself: no new device, no driver to install in the guest,
  and a drive without a shelf answers ILLEGAL REQUEST as it should.
  `cdshelf/cdshelf_proto.h` is the one header for every side (QEMU, the
  DOS program, the Win98/XP program); bump `CDSHELF_PROTO_VERSION` on any
  change, the way doc 14's protocol does.

  **Landed (host side):**
  - `cdshelf/cdshelf_proto.h` — LIST / LOAD / EJECT, a fixed-stride reply
    so the DOS build can walk it with an index register rather than a
    parser, and the flat `<label>\t<path>` shelf-file format.
  - `patches/qemu/52-atapi-disc-shelf.patch` — `ide-cd` gains
    `shelf=<file>`; the opcode lists it, and LOAD/EJECT run the medium
    change from a **bottom half** rather than inline, because changing
    the medium drains and reopens the very drive whose command is still
    executing. That is also how a real drive behaves — the command
    returns, the tray moves after, and the guest sees the change as the
    UNIT ATTENTION `blockdev-change-medium` already raises.
  - The launcher publishes the shelf to `<runtime>/<bundle>-<hash>.shelf`
    beside the monitor socket at spawn *and whenever the shelf changes*,
    so a disc added while the guest runs is in its next listing.

  **Cross-track note:** patch 52 and the ATAPI files are M5's area
  (`patches/qemu/README.md` reserved 52–59 for the CD-ROM backend). This
  is CD-ROM work driven from the launcher track because the shelf is a
  doc 07 feature; the reservation note now says so, and 53–59 stay M5's.

  Verified so far: prepare applies the patch cleanly and idempotently
  (run twice), QEMU builds and `-device ide-cd,help` lists `shelf=`, a
  real player boots with the shelf attached, and
  `tools/atapi-guest-test.py` still passes (164 replies identical to
  `discx`) — so patch 51's behaviour is unregressed in a real DOS guest.
  The vendor command itself was still unexercised at that point; the
  guest programs below are what exercise it (and they changed the device
  side twice in the process).

- **The guest programs landed** (2026-09-05, the other half of the
  feature above). Both speak `cdshelf/cdshelf_proto.h` to the machine's
  own CD-ROM drive; neither installs anything in the guest.

  - `guest-tools/src/cdshelf.c` → `CDSHELF\CDSHELF.EXE`, **one binary for
    Win98 and XP**. The transport is chosen by the OS, not by a build
    flag: SPTI (`IOCTL_SCSI_PASS_THROUGH_DIRECT` on `\\.\<letter>:`) on
    NT, and on 9x `WNASPI32.DLL` loaded with `LoadLibrary` at run time —
    linking it would make the EXE unloadable on XP, where the DLL does
    not exist. Drive selection is the shelf command itself: every CD-ROM
    drive (or every ASPI CD-ROM device) is asked, and the one that
    answers is used, so a machine with two drives needs no argument.
    `-d E:` overrides on NT, `-v` prints every CDB, and the run is
    mirrored into `cdshelf.log`.
  - `guest-tools/src/cdshelf.asm` → `CDSHELF\CDSHELF.COM`, the DOS
    build: PACKET commands by PIO, the same way
    `tools/atapi-guest-test.py` drives the drive (there is no DOS C
    toolchain in this build — `build-wrappers.sh` is a mingw cross build
    and the Open Watcom / DJGPP pieces are skipped). Output through DOS
    function 02h, so `CDSHELF > COM1` and `> FILE` work, which is how the
    test reads it back.
  - Both take `CDSHELF` / `CDSHELF <n>` / `CDSHELF E` and print the same
    listing, with `[in the drive]` and `[missing on the host]` markers.

  **Three things this found, all fixed:**
  - **Patch 52's opcode had to become `CONDDATA`.** Only LIST transfers
    data; LOAD and EJECT do not, and a guest sending them through SPTI or
    ASPI legitimately leaves the byte count limit at zero — which
    `ide_atapi_cmd()`'s generic check aborts at the *ATA* level, before
    the handler runs. LIST now calls `validate_bcl()` itself. Nothing in
    the PIO test could have caught this: it always sets a byte count.
  - **A LOAD of a disc the host cannot open now fails with 02/3A** rather
    than returning GOOD and failing silently in the bottom half, where
    the only trace was a warning on the host's stderr while the guest
    believed the swap had happened. Found by running `CDSHELF 3` against
    the deliberately-missing shelf entry and watching it report success.
  - **The ATAPI signature is not a way to find the drive.** The DOS
    build first looked for 14h/EBh in the cylinder registers; by the time
    a DOS program runs, the BIOS has detected the drive long ago and left
    them at 00/00 (measured under SeaBIOS: status 50h, cylinders 00/00).
    It now asks IDENTIFY PACKET DEVICE, which is the question itself.
    Also: **every CHECK CONDITION must be followed by REQUEST SENSE** —
    the drive reports the same condition to every later command until
    something clears it, so the medium-change poll spun for ever the
    first time. That is exactly what a real driver does, and SPTI/ASPI do
    it for the Windows build.

  **Verified for real, on three guests:**
  - `tools/atapi-guest-test.py` now covers the opcode (19 commands per
    byte-count limit: LIST at five allocation lengths — header-only, all
    four entries, one entry, a partial entry, below the header — with a
    64-byte-truncated label and the MISSING flag checked byte for byte, a
    bad subcommand, a slot past the end, a slot the host cannot open,
    then LOAD/EJECT with **the sectors read before and after**: the shelf
    holds `lec.cue` at slot 0 and `mixed.cue` at slot 1, which differ
    only in that sector 1000 is corrupt on the first, so "the tray really
    changed" is proven by the guest's own reads and not by a status byte.
    206 replies identical to `discx`, up from 164.
  - A **second boot in the same run** puts the real `CDSHELF.COM` on the
    FreeDOS floppy and drives it: list, load 1, list, eject, list, load
    0, list, output captured over COM1, checked for the labels, the
    truncation, the markers and the loads. Error paths (`CDSHELF x`,
    `CDSHELF 99`, a missing disc, a drive with no `shelf=`) were run by
    hand.
  - **Real XP** (`~/vms/winxp.qcow2` through a qcow2 overlay — the user's
    image is never written), booted with an *empty* tray and a shelf
    whose slot 0 is a real ISO: `CDSHELF` listed it over SPTI, `CDSHELF
    0` loaded it, and then `dir D:\` and `type D:\HELLO.TXT` in the same
    guest read the files off it — the whole loop, host shelf to Windows
    reading the disc. Then slot 1, then back to slot 0, each confirmed by
    the `[in the drive]` marker moving and by the files reappearing. A
    load of the missing entry printed "the host cannot reach that disc
    image", and eject left an empty drive that still listed the shelf.

  `tools/cdshelf-guest-test.sh <image> [xp|win98]` is that XP run, kept:
  it generates the ISO and the shelf, builds the EXE, boots the image
  through a qcow2 overlay with an empty tray, drives the guest over QMP
  and prints PASS/FAIL per check. Local only, like the other guest
  scripts — it needs an image, so `scripts/test.sh` will never run it.

  **Win98 (ASPI) — attempted, blocked by the image, not by the code.**
  The path did get real exercise: on `~/vms/win98.qcow2`, `WNASPI32.DLL`
  loads and reports one host adapter (so a stock 98 install does have the
  ASPI layer — the open question in the previous entry is answered), and
  the first run **crashed inside the first `SendASPI32Command`**, which
  found a real bug: ASPI32 is `__cdecl`, not stdcall, and its exports
  carry no `@n` decoration to give that away. Declaring it `WINAPI` puts
  the caller's stack four bytes out on the first call and Windows kills
  the program a moment later. Fixed, but **the fix is not yet confirmed on
  a guest**: every boot of that image since ends with Explorer dying
  before anything of ours runs — the screendump
  (`build/cdshelf-test/win98-run1.png`) shows *"The SHELL32.DLL file is
  linked to missing export SHLWAPI.DLL:…FileAttributesA"*, i.e. that
  install's shell DLLs are mismatched, so there is no Start menu to type
  a command into. Nothing was written to the image (the run is an
  overlay), and repairing someone's Windows install is not this track's
  business. Re-run `tools/cdshelf-guest-test.sh ~/vms/win98.qcow2 win98`
  once that image's shell works, or against a fresh 98 install.

- **Memory and acceleration in the machine form** (user request,
  2026-09-05). Two settings the bundle already carried or wanted, now
  chosen where a machine is created and edited rather than only by
  hand-editing TOML.

  **Memory** (`Machine::ram_mb`, which the form previously preserved but
  never showed): a drag field bounded per family by the new
  `bundle::ram_mb_range` — Win98 32–512 MB, XP 64–3072 — with a
  "Default" button back to doc 06's own value for the family, and a note
  on screen when Win98 is at its ceiling. The bound is doc 06's hard cap,
  not a guess: offering Win9x 2 GB only produces a machine that does not
  boot. Switching family moves the field to that family's default *until
  someone has set a number*, after which it is left alone
  (`Wizard::ram_chosen`).

  **Acceleration** is new: `Machine::accel` = `auto` | `kvm` | `tcg`, and
  `qemu_args` turns it into `-machine pc,accel=…`. **The default is per
  family** (`bundle::default_accel`, user request the same day): **Win98
  is emulated, XP is automatic.** KVM runs the guest at host speed, and
  the `pentium3` model does not protect Win9x from its own fast-CPU bugs
  — it is the speed that trips them — while TCG is also the path docs 13
  and 16's fast paths exist for, so it is what Win98 is actually tuned
  and tested on here. The field is `Option<Accel>` and absent means "this
  family's default" rather than a fixed value, so a bundle written before
  it existed keeps running the way it always did instead of silently
  acquiring KVM; anything the form saves carries an explicit value.
  - `auto` becomes **QEMU's own `kvm:tcg` fallback list**, not a
    `/dev/kvm` probe on our side. A probe's answer can be stale by the
    time the player spawns (permissions, a module unloaded), and QEMU's
    list already means exactly "KVM if you can, emulation otherwise". On
    a non-Linux host it is plain `tcg`, because naming an accelerator
    that does not exist there would print a warning on every boot for
    nothing.
  - `kvm` is the "required" choice, and really does refuse to start
    without KVM — otherwise it would be indistinguishable from `auto`.
  - `tcg` stays first-class: it is the era-CPU behaviour docs 13 and 16's
    x87/SSE fast paths exist for, and the honest setting for Win98, since
    KVM runs the guest at host speed and the `pentium3` *model* does not
    protect against Win9x's fast-CPU bugs — it is the speed that trips
    them. The form says so under the picker when a Win98 machine is set
    to anything but emulation.
  - `player::kvm_available()` (open `/dev/kvm` for writing — the group
    permission is what a bare `exists()` would miss) backs the hint line
    only; nothing in the translated command line depends on it.

  Verified for real, not just rendered. **The command line:** a new XP
  bundle, then `--wizard-edit <bundle> - 1536 kvm`, then `- - tcg`, each
  read back from the TOML and through `--print-args`
  (`-machine pc,accel=kvm:tcg` / `accel=kvm` / `accel=tcg`, `-m 1536`); a
  Win98 machine asked for 4096 MB came back clamped to 512. **The
  defaults:** `--wizard-new win98` writes `accel = "tcg"` / 256 MB and
  `--wizard-new xp` writes `accel = "auto"` / 512 MB, translating to
  `accel=tcg` and `accel=kvm:tcg`; a bundle with the `accel` line
  *deleted* translates the same way for each family. **The accelerator
  actually engaging:** the real `player` binary spawned on each of the
  three settings and asked over the launcher's own QMP socket —
  `query-kvm` returns `enabled: true` for `auto` and `kvm`, and
  `enabled: false` for `tcg`. That is QEMU's own answer from inside the
  in-process embed library, not an inference from the argument list. A
  default Win98 bundle, and a Win98 bundle with no `accel` field at all,
  both boot with `enabled: false` without anyone choosing emulation.
  **The widgets:** a new `--diag-wizard-frame new <family> | edit
  <machine.toml>` verb runs the real form through `egui::Context::run_ui`
  with synthetic clicks (the track's standing practice) — the dumps show
  XP opening at 512 MB and Win98 at 256, the acceleration combo actually
  switching to Emulation (and the Win98 fast-CPU note correctly
  disappearing with it), a typed 384 enabling the "Default" button and
  that button putting 256 back, a family switch moving an untouched value
  and leaving a chosen one at 384, and the edit form opening on the
  stored 1536 MB / "KVM (required)". After the per-family default landed:
  the Win98 form opens on "Emulation" (with no fast-CPU warning, since it
  no longer applies), switching that form to XP moves *both* untouched
  defaults at once (256→512 MB, Emulation→Automatic), and a deliberately
  chosen "KVM (required)" survives the same switch — with its own
  "Default" button now offered next to the picker.

  One bug found by this and fixed: `Wizard::with_new_disk` (the headless
  constructor behind `--wizard-new`) sets the family directly, so it
  never went through the combo box that moves the memory default along
  with it — an XP machine was created with Win98's 256 MB. `build_machine`
  now decides from `ram_chosen` rather than from the field's contents, so
  any constructor that skips the widgets still gets the family's default.

- **CDSHELF grew a face, and always ejects first** (user, 2026-09-05,
  after running it in Win98: *"it worked, but can't you make a gui
  program? it's too unwieldy to use as a terminal command"*, and *"when
  selecting to mount a disc, make it eject the current one first. if i
  tried mounting without ejecting the current one it would do nothing"*).

  **The window** (`run_gui` in `guest-tools/src/cdshelf.c`): the shelf as
  a list, with Insert / Eject / Refresh / Close and a status line. Plain
  USER32 controls created in code — no resource file, no common controls,
  nothing newer than Windows 95 — so the same EXE comes up on a stock
  Win98 and on XP. The EXE is now `-mwindows`: run with no arguments it
  opens the window, and the verbs still work for scripting, which is why
  the listing one had to become explicit (`CDSHELF LIST`; no argument
  means the window now). A verb's output still reaches a redirected
  stdout even in the GUI subsystem — that is what `tools/cdshelf-guest-
  test.sh` relies on, and it still passes.
  The insert runs on a worker thread and posts its result back, because
  a swap takes a second or two and a window that stops painting through
  it looks broken.

  **DOS gets the same idea without a window**: with no arguments
  `CDSHELF.COM` prints the shelf and waits for a key — 0-9 puts that disc
  in the drive, `E` empties it, `R` re-reads the shelf, Esc quits, and
  the listing is reprinted after each. `int 16h`, not DOS input, so the
  menu reads the keyboard whatever stdout is redirected to.

  **Eject-before-insert** is now what "insert" means in both programs and
  the reason is two-fold. The user's half: Windows and MSCDEX cache what
  they last saw in the drive, so a swap they never observed as a removal
  leaves the old disc's files on screen — "it would do nothing". The half
  found while implementing it: the device runs the medium change from a
  *single* bottom half (patch 52), so an eject and a load issued back to
  back without waiting collapse into one and only the last request
  survives. So both programs wait for the drive to report an empty tray
  (TEST UNIT READY answering 02/3A) between the two commands; that wait
  is load-bearing, not politeness.

  Verified on real guests, not just built. **XP:** the window opened over
  the desktop and was screendumped (title "Disc shelf - drive D: (SPTI)",
  both shelf entries listed with the missing one flagged, "2 discs on the
  shelf"), then driven from the keyboard while `query-block` on the
  machine's own QEMU said which file was really in the drive — the medium,
  not a screenshot's word for it. That first run also caught a real bug: a
  plain window keeps the focus itself, so Tab and Enter had nothing to act
  on; the frame now hands focus to the list (`WM_SETFOCUS`) and Insert is
  the default push button, so Enter on the highlighted disc inserts it.
  `tools/cdshelf-guest-test.sh xp` still passes all nine checks with the
  `-mwindows` build and the new eject-first insert. **DOS:** the menu was
  driven by real key presses over QMP (`1`, `e`, `0`, Esc) — each load is
  visible in the reprinted listing with `[in the drive]` moving, and the
  eject in between empties it; `tools/atapi-guest-test.py` still passes
  (206 replies, and its `CDSHELF.COM` stage now drives the `LIST` verb).

## Next steps, in order

1. ~~**The machine bundle format**~~ — done above.
2. ~~**Library grid**~~ — done above.
3. ~~**Spawn a player**~~ — done above.
4. ~~**Guided creation wizard**~~ — done above, including editing an
   existing machine and a native file picker (needs a human
   click-through, see the caveats above).
5. ~~**Snapshots UI + disc-shelf editing**~~ — done above: 5a disc-shelf
   editing (a bundle edit, no IPC), 5b snapshots offline (`qemu-img`),
   5c live media swap and snapshots over the launcher's own `-qmp unix:`
   socket (no protocol, no player change). Windows live control and the
   player's *own* overlay controls (doc 07 puts pause/snapshot/disc swap
   in the player too) are still open.
5b. ~~**The shelf from inside the guest**~~ — done above: the host half
   (patch 52 + the launcher publishing the shelf file) and both guest
   programs, `CDSHELF.EXE` (Win98/XP) and `CDSHELF.COM` (DOS), guarded by
   `tools/atapi-guest-test.py` and, for Windows,
   `tools/cdshelf-guest-test.sh`. Open: **the Win98 (ASPI) run**, blocked
   on `~/vms/win98.qcow2`'s broken shell rather than on the code — see
   the entry above. `tools/cdshelf-guest-test.sh ~/vms/win98.qcow2
   win98` is the command; it needs no changes, only a 98 install whose
   Explorer starts.
6. **Packaging** (last, per doc 08 M6): signed macOS .app + notarization,
   Windows installer + portable zip, Linux Flatpak — the M6 exit
   criterion ("stranger installs → plays a disc dump with a CRT shader
   in under an hour") needs both binaries and this step.

No wired-in test tool exists yet for this track; CLAUDE.md's
integration/e2e policy still applies once there's a real boundary worth
guarding against regressions — don't add `#[cfg(test)]` modules.
`launcher`'s debug verbs (`--new`, `--print-args`, `--play`,
`--wizard-new`, `--wizard-edit`, `--pick-file`, `--new-shader-profile`,
`--set-shader-param`, `--list-shader-params`, `--assign-shader`,
`--print-shader-args`, `--preview-shader`, `--diag-preview-frame`,
`--diag-editor-frame`, `--disc-shelf`, `--diag-shelf-frame`,
`--snapshots` (`--live` for a running machine), `--diag-snapshots-frame`,
`--qmp-socket`, `--insert-disc`, `--kvm`, `--diag-wizard-frame`; and
`--wizard-edit` now takes optional `[ram-mb] [auto|kvm|tcg]`, `-` keeping
a field) were
exercised by hand
this session (see the state notes above) rather than wired into
`scripts/test.sh`, because doing that from `scripts/test.sh`
needs a `build/qemu` in whichever worktree runs it — this one doesn't
have one (the shared-checkout `QEMU_EMBED_LIB_DIR` trick used above is a
manual convenience, not something a checked-in script should depend on
across worktrees). Once this track's worktree does a real
`prepare-qemu.sh && configure-qemu.sh && ninja`, add a `launcher` check
to `scripts/test.sh`: `--new` a bundle against a fresh empty qcow2,
`--play` it, confirm a frame via `PLAYER_DUMP_OUT` (needs `--shader` per
the player's source — plain `PLAYER_DUMP` or a screendump-equivalent may
be simpler for a bundle-only check) — no OS install required, just a
BIOS/iPXE splash — then kill the spawned process (synthetic disk, no
guest, no dirty-FAT concern) and add the row to `CLAUDE.md`'s testing
table too.
