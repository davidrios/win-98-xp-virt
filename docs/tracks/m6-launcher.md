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
  0.36.1; `main.rs` opens a native window (`eframe::run_native`) with an
  empty-state central panel ("No machines yet.", a disabled "New
  machine…" button). No machine bundle format, thumbnails, or scanning
  yet — this is the M0 stub replaced with a real (if empty) window.
  Verified: `cargo build` clean (no warnings), the binary runs against
  the box's Wayland session for 5 s with no crash (RADV's usual
  non-conformant-ICD notice only).

## Next steps, in order

1. **The machine bundle format**: pin down `machine.toml`'s shape (doc 06
   has the reference machine definitions this should read/write) —
   family (Win98/XP), disk path(s), disc shelf, RAM, the shader preset
   override, grab behavior. This unblocks everything else below.
2. **Library grid**: scan a configured library directory for bundles,
   render as a grid (name, family badge, running state; thumbnails come
   later — decide the thumbnail source, e.g. `PLAYER_DUMP_OUT` style
   headless capture on save, once the player side exists).
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

No test tooling exists yet for this track; CLAUDE.md's integration/e2e
policy still applies once there's a real boundary to test (e.g. a bundle
round-trip, or the launcher successfully spawning and detecting a player
exit) — don't add `#[cfg(test)]` modules.
