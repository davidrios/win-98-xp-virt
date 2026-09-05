//! The machine library (doc 07): a directory of bundle subdirectories,
//! each holding one `machine.toml`. "A plain, documented directory layout
//! the user can back up" (doc 07's settings taxonomy) — no database, no
//! hidden index; the grid is just a scan.

use crate::bundle::{Family, Machine};
use std::path::{Path, PathBuf};

pub const BUNDLE_FILE: &str = "machine.toml";

/// The default library directory: the platform data dir (`~/.local/share`
/// on Linux, `~/Library/Application Support` on macOS, `%APPDATA%` on
/// Windows) plus `machines`. `LAUNCHER_LIBRARY_DIR` overrides it (dev/test
/// convenience, matching the project's `PLAYER_*` env-knob convention).
pub fn default_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("LAUNCHER_LIBRARY_DIR") {
        return dir.into();
    }
    directories::ProjectDirs::from("", "", "win98-xp-virt")
        .map(|d| d.data_dir().join("machines"))
        .unwrap_or_else(|| PathBuf::from("machines"))
}

pub struct LibraryEntry {
    pub dir: PathBuf,
    pub machine: Machine,
}

/// A directory name from a machine name: lowercase, non-alphanumerics
/// collapsed to `-`, deduplicated against what's already in `dir`.
fn slug(dir: &Path, name: &str) -> String {
    let mut base: String = name
        .to_lowercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    while base.contains("--") {
        base = base.replace("--", "-");
    }
    let base = base.trim_matches('-');
    let base = if base.is_empty() { "machine" } else { base };
    let mut candidate = base.to_string();
    let mut n = 2;
    while dir.join(&candidate).exists() {
        candidate = format!("{base}-{n}");
        n += 1;
    }
    candidate
}

/// Create a new bundle under `dir` from doc 06's reference defaults,
/// returning the path to its `machine.toml`.
pub fn create(dir: &Path, family: Family, name: String, disk: PathBuf) -> std::io::Result<PathBuf> {
    std::fs::create_dir_all(dir)?;
    let machine_dir = dir.join(slug(dir, &name));
    std::fs::create_dir_all(&machine_dir)?;
    let bundle_path = machine_dir.join(BUNDLE_FILE);
    Machine::reference(family, name, disk).save(&bundle_path)?;
    Ok(bundle_path)
}

/// Every bundle directly under `dir` (one level, not recursive). A
/// subdirectory without a readable `machine.toml` is skipped, not fatal —
/// one corrupt bundle shouldn't take down the whole grid.
pub fn scan(dir: &Path) -> Vec<LibraryEntry> {
    let Ok(read) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut entries: Vec<LibraryEntry> = read
        .filter_map(|e| e.ok())
        .filter(|e| e.file_type().map(|t| t.is_dir()).unwrap_or(false))
        .filter_map(|e| {
            let dir = e.path();
            match Machine::load(&dir.join(BUNDLE_FILE)) {
                Ok(machine) => Some(LibraryEntry { dir, machine }),
                Err(err) => {
                    eprintln!("[library] skipping {}: {err}", dir.display());
                    None
                }
            }
        })
        .collect();
    entries.sort_by(|a, b| a.machine.name.cmp(&b.machine.name));
    entries
}
