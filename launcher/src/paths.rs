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
/// (`share/2ksbox`, `lib/2ksbox`, …) and the launcher's installed
/// executable. `win98-xp-virt` is the repository's working name and still
/// names the checkout, the docs and the user's data directory (renaming
/// that one needs a migration, deliberately deferred — 2026-09-05).
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
    let prefix = exe.parent()?.parent()?;
    prefix.join("share").join(NAME).is_dir().then(|| prefix.to_path_buf())
}

/// A companion at `installed` under the install prefix, or at `checkout`
/// in the source tree — whichever layout this binary is running in.
/// Always returns a path; whether it exists is the caller's problem,
/// since for most of these "missing" is a state the UI already reports
/// (no guest-tools ISO built, no preset collection yet).
pub fn resource(installed: &str, checkout_rel: &str) -> PathBuf {
    match install_prefix() {
        Some(prefix) => prefix.join(installed),
        None => checkout(checkout_rel),
    }
}

/// `rel` in the workspace checkout this binary was built from.
pub fn checkout(rel: &str) -> PathBuf {
    Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/..")).join(rel)
}
