//! The disc shelf (doc 07): the user's collection of disc images, shared
//! by every machine rather than owned by one.
//!
//! It started per-machine (`Machine::discs`) and that was wrong: a rip of
//! Blood disc 2 is a property of the person, not of the XP box that
//! happened to install it first. Two machines wanting the same disc had
//! to list it twice, and a disc added while setting one machine up was
//! invisible to the next. So the shelf lives here — one flat
//! `discs.toml` next to the machine and shader-profile libraries — and
//! all a machine keeps is `Machine::disc`, the one disc in its drive at
//! boot. Anything else is inserted at runtime (`control.rs`).
//!
//! Entries are labelled because that's the other half of the point: a
//! shelf of `d1.cue`, `disc2.cue`, `cd1.iso` is not a library. The label
//! defaults to the file name and is editable.

use crate::filepicker;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const DISC_FILTER: filepicker::Filter = ("Disc images", &["iso", "cue", "ccd", "mds"]);

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Disc {
    /// What to call it in a list. Defaults to the file name.
    pub label: String,
    pub path: PathBuf,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct DiscLibrary {
    #[serde(default, rename = "disc")]
    pub discs: Vec<Disc>,
}

/// `<platform data dir>/discs.toml`, beside `machines/` and
/// `shader-profiles/` (doc 07: "bundles live in a plain, documented
/// directory layout the user can back up"). `LAUNCHER_DISC_LIBRARY`
/// overrides it, matching the project's `PLAYER_*`/`LAUNCHER_*` env-knob
/// convention.
pub fn default_path() -> PathBuf {
    if let Ok(path) = std::env::var("LAUNCHER_DISC_LIBRARY") {
        return path.into();
    }
    directories::ProjectDirs::from("", "", "win98-xp-virt")
        .map(|d| d.data_dir().join("discs.toml"))
        .unwrap_or_else(|| PathBuf::from("discs.toml"))
}

/// A label for a disc that has none: the file name without its
/// extension, which is what a rip is usually named after.
pub fn default_label(path: &Path) -> String {
    path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| path.display().to_string())
}

impl DiscLibrary {
    /// Read the shelf. A missing file is an empty shelf, not an error —
    /// that's just a fresh install. A *corrupt* one is an error, so a
    /// hand-edit gone wrong is reported instead of silently discarding
    /// the collection by overwriting it with an empty one.
    pub fn load(path: &Path) -> std::io::Result<DiscLibrary> {
        match std::fs::read_to_string(path) {
            Ok(text) => toml::from_str(&text).map_err(std::io::Error::other),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(DiscLibrary::default()),
            Err(e) => Err(e),
        }
    }

    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let text = toml::to_string_pretty(self).map_err(std::io::Error::other)?;
        std::fs::write(path, text)
    }

    pub fn position(&self, path: &Path) -> Option<usize> {
        self.discs.iter().position(|d| d.path == path)
    }

    /// Put a disc on the shelf. Returns false if it was already there —
    /// the same image added twice is one entry, not two rows that then
    /// disagree about their labels.
    pub fn add(&mut self, path: PathBuf) -> bool {
        if self.position(&path).is_some() {
            return false;
        }
        self.discs.push(Disc { label: default_label(&path), path });
        true
    }

    /// Pull `machine.toml`'s legacy per-machine `discs` lists onto the
    /// shared shelf, so nothing a user added before this change is lost.
    /// Idempotent (`add` deduplicates by path), so it can run after every
    /// library scan. Returns how many were new.
    pub fn import_legacy(&mut self, machines: &[crate::library::LibraryEntry]) -> usize {
        let mut added = 0;
        for entry in machines {
            for disc in &entry.machine.discs {
                if self.add(disc.clone()) {
                    added += 1;
                }
            }
        }
        added
    }
}

/// Write the shelf in the flat form QEMU's ATAPI handler reads
/// (`cdshelf/cdshelf_proto.h`): one `<label>\t<path>` line per disc.
///
/// Not `discs.toml` itself, because QEMU's side of this is C and a
/// tab-separated line file is a parser you can read in one sitting.
/// Labels have tabs and newlines replaced rather than being rejected —
/// the user typed a label, not a record separator, and losing their disc
/// over a stray tab would be absurd. A *path* containing a newline is
/// skipped instead: there is no safe way to write it in this format, and
/// silently truncating it would point the guest at the wrong file.
pub fn write_shelf_file(library: &DiscLibrary, path: &Path) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let mut out = String::new();
    for disc in library.discs.iter().take(crate::disc_library::MAX_SHELF_ENTRIES) {
        let path_str = disc.path.display().to_string();
        if path_str.contains('\n') {
            eprintln!("[discs] skipping {path_str:?}: a newline in a path can't be written to the shelf file");
            continue;
        }
        let label = disc.label.replace(['\t', '\n', '\r'], " ");
        out.push_str(&label);
        out.push('\t');
        out.push_str(&path_str);
        out.push('\n');
    }
    std::fs::write(path, out)
}

/// Matches `CDSHELF_FILE_MAX_ENTRIES` in `cdshelf/cdshelf_proto.h`: the
/// guest walks the reply with a fixed stride and a bounded buffer.
pub const MAX_SHELF_ENTRIES: usize = 256;

/// The newest guest-tools ISO (`guest-tools/build-wrappers.sh` writes
/// `guest-tools/out/guest-tools-3dfx-<date>.iso`), for doc 07's
/// "one-click guest-tools ISO attach". Found relative to the checkout
/// this binary was *built* from, like `player::pc_bios_dir` — shipping a
/// copy is a packaging (M6 step 6) concern. `LAUNCHER_GUEST_TOOLS_ISO`
/// overrides it with an explicit path.
pub fn guest_tools_iso() -> Option<PathBuf> {
    if let Ok(path) = std::env::var("LAUNCHER_GUEST_TOOLS_ISO") {
        return Some(path.into());
    }
    let dir = PathBuf::from(concat!(env!("CARGO_MANIFEST_DIR"), "/../guest-tools/out"));
    let mut candidates: Vec<(std::time::SystemTime, PathBuf)> = std::fs::read_dir(dir)
        .ok()?
        .filter_map(|e| e.ok())
        .filter(|e| {
            let name = e.file_name();
            let name = name.to_string_lossy();
            name.starts_with("guest-tools-") && name.ends_with(".iso")
        })
        .filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path())))
        .collect();
    candidates.sort();
    // Canonicalized because this one is *stored*: the build-time anchor
    // is `<manifest>/../guest-tools/out`, and a `launcher/../guest-tools`
    // on the shelf would be correct but unreadable.
    candidates.pop().map(|(_, path)| path.canonicalize().unwrap_or(path))
}
