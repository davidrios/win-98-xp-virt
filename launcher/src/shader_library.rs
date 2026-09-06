//! The shader profile library: a directory of `<slug>.toml` files, one per
//! profile — flat, unlike the machine library's per-bundle subdirectories
//! (`library.rs`), since a profile has no disk image or disc shelf beside
//! it to keep together. Same "plain, documented directory, no database"
//! stance (doc 07).

use crate::shader_profile::ShaderProfile;
use std::path::{Path, PathBuf};

/// The default profile directory: the platform data dir plus
/// `shader-profiles`, alongside `library::default_dir`'s `machines`.
/// `LAUNCHER_SHADER_PROFILES_DIR` overrides it.
pub fn default_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("LAUNCHER_SHADER_PROFILES_DIR") {
        return dir.into();
    }
    directories::ProjectDirs::from("", "", "win98-xp-virt")
        .map(|d| d.data_dir().join("shader-profiles"))
        .unwrap_or_else(|| PathBuf::from("shader-profiles"))
}

pub struct ProfileEntry {
    pub path: PathBuf,
    pub profile: ShaderProfile,
}

/// The profile id a machine's `shader_profile` field stores: a `.toml`
/// file's bare stem (e.g. `trinitron-warm`), matching `path`'s own name so
/// looking one up by id (`find`, below) is a plain filename join.
pub fn id_of(path: &Path) -> String {
    path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default()
}

/// An id from a profile name: lowercase, non-alphanumerics collapsed to
/// `-`, deduplicated against what's already in `dir` — same scheme as
/// `library::slug`, just against `<candidate>.toml` files instead of
/// bundle subdirectories.
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
    let base = if base.is_empty() { "profile" } else { base };
    let mut candidate = base.to_string();
    let mut n = 2;
    while dir.join(format!("{candidate}.toml")).exists() {
        candidate = format!("{base}-{n}");
        n += 1;
    }
    candidate
}

/// Create a new profile under `dir`, returning its file path.
pub fn create(dir: &Path, name: String, preset: PathBuf) -> std::io::Result<PathBuf> {
    std::fs::create_dir_all(dir)?;
    let path = dir.join(format!("{}.toml", slug(dir, &name)));
    ShaderProfile::new(name, preset).save(&path)?;
    Ok(path)
}

/// Every `*.toml` directly under `dir`. A file that fails to parse is
/// skipped with a stderr line, not fatal — matches `library::scan`.
pub fn scan(dir: &Path) -> Vec<ProfileEntry> {
    let Ok(read) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut entries: Vec<ProfileEntry> = read
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|ext| ext == "toml"))
        .filter_map(|path| match ShaderProfile::load(&path) {
            Ok(profile) => Some(ProfileEntry { path, profile }),
            Err(err) => {
                eprintln!("[shader-library] skipping {}: {err}", path.display());
                None
            }
        })
        .collect();
    entries.sort_by(|a, b| a.profile.name.cmp(&b.profile.name));
    entries
}

/// Look up a profile by id (a machine's `shader_profile` value) under
/// `dir`. `None` covers both "no such profile" and "unreadable" — a
/// dangling reference (the profile was deleted after a machine picked it)
/// falls back to no shader override rather than failing the machine.
pub fn find(dir: &Path, id: &str) -> Option<ShaderProfile> {
    ShaderProfile::load(&dir.join(format!("{id}.toml"))).ok()
}

pub fn delete(path: &Path) -> std::io::Result<()> {
    std::fs::remove_file(path)
}
