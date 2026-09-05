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

## Next steps, in order

1. ~~**The machine bundle format**~~ — done above.
2. ~~**Library grid**~~ — done above.
3. ~~**Spawn a player**~~ — done above.
4. ~~**Guided creation wizard**~~ — done above, including editing an
   existing machine and a native file picker (needs a human
   click-through, see the caveats above).
5. **Snapshots UI + disc-shelf editing**: once the player exposes QMP
   snapshot/media-change operations for the launcher to drive (may need
   a small IPC surface between the two binaries — design it when this
   item is reached, not before).
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
`--print-shader-args`) were exercised by hand
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
