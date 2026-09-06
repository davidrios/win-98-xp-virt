//! The launcher, minus the drawing.
//!
//! There are two front ends over this crate — `launcher/` (egui) and
//! `launcher-qt/` (Qt 6 / QML through cxx-qt), both maintained, doc 07 —
//! and the rule that keeps them honest is that **everything either of
//! them could disagree about lives here**. Not just the file formats and
//! the subprocesses: the *windows' own behaviour* too. Which memory
//! default follows the family until someone picks a number, the exact
//! sentence under the networking checkbox, when a snapshot job is
//! polled, whether a running machine is driven through its monitor or
//! through `qemu-img` — all of that is one implementation here, and a
//! front end is the widgets that show it plus the events that call in.
//!
//! The split it replaced was `#[path]`-including ten files from
//! `launcher/src/` into the Qt crate, which proved the *file formats*
//! were portable but left every window's state machine written twice.
//! They had already drifted: the Qt wizard had no processor, floppy or
//! boot field and its networking checkbox didn't follow the family; its
//! "no network adapter" line said `Windows` where egui's said `the
//! guest`; and saving a *new* shader profile dropped the parameter
//! overrides on the egui side and kept them on the Qt side. Those are
//! all one piece of code now, so there is nothing left to drift.
//!
//! Three groups of modules:
//!
//! * **The data.** `bundle` (`machine.toml`), `library` (the machine
//!   library), `disc_library` (the shared disc shelf), `shader_profile` /
//!   `shader_library` / `shader_source` (profiles, their library, and
//!   fetching the preset collection), `paths` (where everything lives,
//!   installed or in a checkout).
//! * **The machinery.** `player` (spawning one, and `qemu-img`),
//!   `control` (QMP to a running machine), `snapshots` (`qemu-img`'s
//!   half of the same), `preview` (the shader chain on a still image).
//! * **The window models.** `machines`, `wizard`, `shelf`, `snaps`,
//!   `editor` — one per window, holding its whole state machine, and
//!   `browse` for the one file-dialog decision that is not the dialog.
//!   `cli` is every debug verb that needs no toolkit, so both binaries
//!   answer the same ones identically.

pub mod browse;
pub mod bundle;
pub mod cli;
pub mod console;
pub mod control;
pub mod disc_library;
pub mod editor;
// What this host's GPU can do for the Direct3D executor (ADR-013).
pub mod host_gpu;
pub mod library;
pub mod machines;
pub mod paths;
pub mod player;
pub mod preview;
pub mod shader_library;
pub mod shader_profile;
pub mod shader_source;
pub mod shelf;
pub mod snapshots;
pub mod snaps;
pub mod wizard;
