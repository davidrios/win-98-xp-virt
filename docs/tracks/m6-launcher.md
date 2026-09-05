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

## Next steps, in order

1. ~~**The machine bundle format**~~ — done above.
2. ~~**Library grid**~~ — done above.
3. **Spawn a player**: launch `player` as a child process pointed at a
   bundle; surface its running/exited state back in the grid.
4. **Guided creation wizard**: family → name → disk size → install media
   → bundle written from doc 06's reference definitions; an advanced
   drawer that edits the raw TOML (never a QEMU command line, per doc 07).
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
guarding against regressions — don't add `#[cfg(test)]` modules. The
bundle format's `--new`/`--print-args` pair was exercised by hand this
session (see above) rather than wired into `scripts/test.sh`, since the
real end-to-end boundary — a bundle actually booting a guest to the
BIOS/POST screen — needs `build/qemu` prepared in this worktree, which
step 3 (spawning a player) will need anyway. Do that build then, and add
a check at that point (e.g. `launcher --new` a bundle against a fresh
empty qcow2, spawn `player -- $(launcher --print-args …)`, confirm a
frame via `PLAYER_DUMP_OUT` — no OS install required, just a BIOS
splash) rather than before.
