//! Where the launcher's companions are: the `player` binary, our
//! `qemu-img`, QEMU's `pc-bios` firmware, the guest-tools ISO, a shipped
//! shader collection. Every one of them lived in the checkout this binary
//! was *built* from until packaging (M6 step 6) had to exist; an
//! installed launcher has no checkout at all.
//!
//! So there are two layouts, asked in this order (an explicit
//! `LAUNCHER_*` environment variable still wins over both — each caller
//! checks its own first, as it always did):
//!
//! 1. **Installed** — the layout `scripts/package-linux.sh` stages and
//!    doc 07 documents, found *relative to the running executable*
//!    (`<exe dir>/..`), so the whole tree can be moved or extracted
//!    anywhere and still work. `share/2ksbox` is the marker: a
//!    launcher that merely happens to sit in some `bin/` is not
//!    installed.
//! 2. **A source checkout** — `CARGO_MANIFEST_DIR`, baked in at compile
//!    time (like `qemu-embed/build.rs`'s own default). Not the process's
//!    current working directory, which a bare relative path would be and
//!    which isn't guaranteed to be the workspace root — a real "No such
//!    file or directory" the user hit running the launcher from
//!    elsewhere.
//!
//! It is one or the other, never a mixture: an installed launcher answers
//! only with its own prefix, even for a file the package left out. The
//! alternative — falling through to the checkout — would mean a package
//! tested on a developer's machine silently works there and fails
//! everywhere else, which is exactly the bug packaging exists to catch.
//! A missing file inside the prefix is reported as missing, by whichever
//! window wanted it.

use std::path::{Path, PathBuf};
use std::sync::OnceLock;

/// The product name: the resource directories inside a prefix
/// (`share/2ksbox`, `lib/2ksbox`, …), the launcher's installed executable
/// and the user's own data directory. `win98-xp-virt` was the working
/// name until 2026-09-06 and survives only in `migrate_data_dir` below.
pub const NAME: &str = "2ksbox";

/// The application ID: the desktop entry's filename, the icon's name, the
/// Wayland `app_id` the compositor matches between the two, and the
/// Flatpak/AppStream ID. Reverse-DNS of `2ksbox.com` — with the leading
/// digit escaped as `_2ksbox`, because a name segment may not start with
/// one (`flatpak build-init` rejects `com.2ksbox.…` outright; the same
/// convention gives `7-zip.org` `org._7zip.…`).
pub const APP_ID: &str = "com._2ksbox.Launcher";

/// The prefix this launcher is installed under, or `None` when it is a
/// binary in a checkout's `target/`. Computed once: it is a couple of
/// `stat`s, but callers ask per frame.
pub fn install_prefix() -> Option<&'static Path> {
    static PREFIX: OnceLock<Option<PathBuf>> = OnceLock::new();
    PREFIX.get_or_init(detect_prefix).as_deref()
}

fn detect_prefix() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    if cfg!(windows) {
        // A Windows package is one folder the user unzips and opens: the
        // executables at the top, the DLLs beside them (which is where
        // the loader looks), the data directories under it. So the prefix
        // is the executable's own directory, and `pc-bios` is the marker
        // — the one directory every package has and nothing else would
        // put next to a stray copy of the launcher.
        let dir = exe.parent()?;
        return dir.join("pc-bios").is_dir().then(|| dir.to_path_buf());
    }
    let prefix = exe.parent()?.parent()?;
    prefix.join("share").join(NAME).is_dir().then(|| prefix.to_path_buf())
}

/// A companion's place inside the prefix. Unix keeps the
/// `bin`/`lib`/`libexec`/`share` split doc 07 documents; a Windows
/// package is flat, so the same name loses the directory that only
/// existed to keep a Unix prefix tidy (`share/2ksbox/pc-bios` →
/// `pc-bios`). Written once here rather than at every call site, so both
/// layouts are described by the same string.
fn in_prefix(installed: &str) -> &str {
    if !cfg!(windows) {
        return installed;
    }
    for lead in ["share/2ksbox/", "lib/2ksbox/", "libexec/2ksbox/", "bin/"] {
        if let Some(rest) = installed.strip_prefix(lead) {
            return rest;
        }
    }
    installed
}

/// A companion at `installed` under the install prefix, or at `checkout`
/// in the source tree — whichever layout this binary is running in.
/// Always returns a path; whether it exists is the caller's problem,
/// since for most of these "missing" is a state the UI already reports
/// (no guest-tools ISO built, no preset collection yet).
pub fn resource(installed: &str, checkout_rel: &str) -> PathBuf {
    match install_prefix() {
        Some(prefix) => prefix.join(in_prefix(installed)),
        None => checkout(checkout_rel),
    }
}

/// `rel` in the workspace checkout this binary was built from. A Windows
/// binary built from a Linux checkout (docs/build-windows.md) finds
/// QEMU's own artefacts under `build/win/qemu`, since that checkout holds
/// both builds at once.
pub fn checkout(rel: &str) -> PathBuf {
    let root = Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/.."));
    if cfg!(windows) {
        if let Some(rest) = rel.strip_prefix("build/qemu") {
            return root.join(format!("build/win/qemu{rest}"));
        }
    }
    root.join(rel)
}

/// The user's own directory: `machines/`, `discs.toml`, `shader-profiles/`
/// and a downloaded preset collection (`~/.local/share/2ksbox` on Linux,
/// `~/Library/Application Support/2ksbox` on macOS, `%APPDATA%\2ksbox` on
/// Windows). `None` only where the platform has no home directory at all,
/// which every caller answers with a bare relative path.
///
/// It was `win98-xp-virt` until the repository took the product's name
/// (ADR-011, amended 2026-09-06), so an existing library is **moved once**
/// here, the first time anything asks: a plain rename inside the same
/// parent directory, atomic, and only when the new name does not exist
/// yet. A user who upgrades finds their machines where they left them
/// without knowing any of this happened; one who has both directories
/// (two versions run side by side) keeps them both, and is told which one
/// is now being used rather than having them merged behind their back.
pub fn data_dir() -> Option<&'static Path> {
    static DIR: OnceLock<Option<PathBuf>> = OnceLock::new();
    DIR.get_or_init(|| {
        let dir = directories::ProjectDirs::from("", "", NAME)?.data_dir().to_path_buf();
        migrate_data_dir(&dir);
        Some(dir)
    })
    .as_deref()
}

/// The `win98-xp-virt` → `2ksbox` move, done once. Every failure is a
/// warning and nothing else: the launcher still starts, on an empty
/// library, which is recoverable by hand — refusing to run would not be.
fn migrate_data_dir(new: &Path) {
    let Some(old) = directories::ProjectDirs::from("", "", "win98-xp-virt").map(|d| d.data_dir().to_path_buf()) else {
        return;
    };
    if old == new || !old.is_dir() {
        return;
    }
    if new.exists() {
        eprintln!("launcher: both {} and {} exist; using the latter", old.display(), new.display());
        return;
    }
    if let Some(parent) = new.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    match std::fs::rename(&old, new) {
        Ok(()) => eprintln!("launcher: moved {} to {}", old.display(), new.display()),
        Err(e) => eprintln!("launcher: cannot move {} to {}: {e}", old.display(), new.display()),
    }
}

/// The platform runtime directory (`/run/user/<uid>/2ksbox` on Linux) for
/// this machine's monitor socket, or the temp dir where there is none.
/// Nothing is migrated: what lives here belongs to a running process.
pub fn runtime_dir() -> PathBuf {
    directories::ProjectDirs::from("", "", NAME)
        .and_then(|d| d.runtime_dir().map(Path::to_path_buf))
        .unwrap_or_else(std::env::temp_dir)
}
