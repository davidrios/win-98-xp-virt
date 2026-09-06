//! Where the shader *presets* come from — the collection itself, not the
//! profiles built on top of it (`shader_library.rs`).
//!
//! In a source checkout they are the `third_party/slang-shaders`
//! submodule. Someone who cloned without `--recurse-submodules`, or who
//! one day installs a packaged launcher, has no such directory — and a
//! profile manager whose preset picker opens on nothing is a dead end.
//! So the collection can also be **downloaded**: upstream's tarball,
//! unpacked into the platform data directory beside `machines/` and
//! `shader-profiles/`. Never into `third_party/`, which belongs to git.

use std::io::Read;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

/// libretro's own collection, the same repository the `third_party`
/// submodule points at, as a tarball of the current `master` — codeload
/// is what a `git clone` would fetch from anyway. Not pinned to the
/// submodule's commit: the binary that downloads this may be a packaged
/// launcher with no repository to read a pin out of, and presets are
/// additive (a profile stores parameter overrides by name and tolerates
/// the preset gaining new ones — see `shader_profile`).
const TARBALL_URL: &str = "https://codeload.github.com/libretro/slang-shaders/tar.gz/refs/heads/master";

/// Roughly what that tarball weighs, for the button to say so before
/// someone commits to it on a phone tether. Approximate on purpose.
pub const DOWNLOAD_SIZE: &str = "~50 MB";

/// The collection that came with this build: the checkout's
/// `third_party/slang-shaders` submodule, or — for an installed launcher
/// — whatever the package shipped in `share/2ksbox/shaders`.
/// `scripts/package-linux.sh` ships none by default (80 MB, and the
/// manager can fetch them), but `--with-shaders`, a Flatpak or a distro
/// package would, and then nobody should be asked to download what they
/// already have.
pub fn repo_dir() -> PathBuf {
    crate::paths::resource("share/2ksbox/shaders", "third_party/slang-shaders")
}

/// Where a download lands, and the first place looked at when
/// `LAUNCHER_SHADERS_DIR` names it: the platform data dir plus
/// `shaders`, alongside `machines/` and `shader-profiles/`.
pub fn install_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("LAUNCHER_SHADERS_DIR") {
        return dir.into();
    }
    crate::paths::data_dir().map(|d| d.join("shaders")).unwrap_or_else(|| PathBuf::from("shaders"))
}

/// The preset collection on this machine, or `None` if there isn't one —
/// which is what puts the "Download presets" button on screen.
///
/// `LAUNCHER_SHADERS_DIR` is an explicit statement about where the
/// presets are, so when it is set nothing else is consulted. Otherwise
/// the collection this build came with (`repo_dir`) wins over a
/// downloaded copy: in a checkout it is the submodule a developer's
/// `--shader third_party/slang-shaders/…` paths and this repo's docs
/// already refer to, and in a package it is the one the package can
/// promise is there.
pub fn presets_dir() -> Option<PathBuf> {
    if std::env::var_os("LAUNCHER_SHADERS_DIR").is_some() {
        let dir = install_dir();
        return has_presets(&dir).then_some(dir);
    }
    let repo = repo_dir();
    if has_presets(&repo) {
        return Some(repo);
    }
    let installed = install_dir();
    has_presets(&installed).then_some(installed)
}

/// Whether `dir` looks like a preset collection: at least one `.slangp`
/// within two levels (upstream keeps them one directory down —
/// `crt/crt-lottes.slangp` — with a few at the top). Cheap enough for
/// the ~50 directories that tree has, and the caller caches the answer
/// rather than asking per frame. An empty or half-unpacked directory
/// correctly reads as "no presets".
pub fn has_presets(dir: &Path) -> bool {
    any_preset(dir, 2)
}

fn any_preset(dir: &Path, depth: u32) -> bool {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return false;
    };
    let mut subdirs = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().is_some_and(|e| e.eq_ignore_ascii_case("slangp")) {
            return true;
        }
        if depth > 0 && entry.file_type().is_ok_and(|t| t.is_dir()) {
            subdirs.push(path);
        }
    }
    subdirs.iter().any(|d| any_preset(d, depth - 1))
}

/// What the UI shows about a download in flight.
pub enum Status {
    /// Bytes of the tarball read so far. There is no total: codeload
    /// streams the archive and sends no `Content-Length`, so a
    /// percentage would have to be invented.
    Running(u64),
    Done(PathBuf),
    Failed(String),
}

/// A download running on its own thread. Dropping this doesn't cancel it
/// — the thread finishes writing and exits; nothing it touches outside
/// its staging directory is visible until the final rename.
pub struct Download {
    bytes: Arc<AtomicU64>,
    result: Arc<Mutex<Option<Result<PathBuf, String>>>>,
}

impl Download {
    /// Fetch and unpack the collection into `dest`, replacing whatever
    /// is there. Returns immediately.
    pub fn start(dest: PathBuf) -> Download {
        let bytes = Arc::new(AtomicU64::new(0));
        let result = Arc::new(Mutex::new(None));
        let (b, r) = (bytes.clone(), result.clone());
        std::thread::spawn(move || {
            let outcome = fetch(&dest, &b).map(|()| dest).map_err(|e| e.to_string());
            *r.lock().unwrap() = Some(outcome);
        });
        Download { bytes, result }
    }

    pub fn status(&self) -> Status {
        match self.result.lock().unwrap().clone() {
            None => Status::Running(self.bytes.load(Ordering::Relaxed)),
            Some(Ok(dir)) => Status::Done(dir),
            Some(Err(e)) => Status::Failed(e),
        }
    }
}

/// The whole job, synchronously: used by the thread above and, directly,
/// by `main.rs`'s `--download-shaders` verb so the real fetch and unpack
/// can be exercised without a window.
pub fn fetch(dest: &Path, bytes: &AtomicU64) -> std::io::Result<()> {
    // Unpack beside the destination and rename only once the whole
    // archive is out: an interrupted download must not leave a
    // half-collection that `has_presets` would then call installed.
    let staging = staging_dir(dest);
    if staging.exists() {
        std::fs::remove_dir_all(&staging)?;
    }
    if let Some(parent) = staging.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let response = ureq::get(TARBALL_URL)
        .call()
        .map_err(|e| std::io::Error::other(format!("{TARBALL_URL}: {e}")))?;
    let counted = Counting { inner: response.into_body().into_reader(), bytes };
    unpack(counted, &staging)?;
    if !has_presets(&staging) {
        std::fs::remove_dir_all(&staging).ok();
        return Err(std::io::Error::other("the downloaded archive contained no .slangp presets"));
    }
    // Replace the old collection only now, and only after the new one is
    // known good. The old directory goes out of the way first because a
    // rename onto a non-empty directory fails.
    let previous = staging.with_extension("previous");
    std::fs::remove_dir_all(&previous).ok();
    if dest.exists() {
        std::fs::rename(dest, &previous)?;
    }
    std::fs::rename(&staging, dest)?;
    std::fs::remove_dir_all(&previous).ok();
    Ok(())
}

fn staging_dir(dest: &Path) -> PathBuf {
    let name = dest.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "shaders".into());
    dest.with_file_name(format!("{name}.part"))
}

/// Unpack the gzipped tar into `into`, dropping the archive's single
/// top-level directory (`slang-shaders-master/`) so the presets land at
/// `into/crt/…` rather than one level down.
fn unpack(reader: impl Read, into: &Path) -> std::io::Result<()> {
    let mut archive = tar::Archive::new(flate2::read::GzDecoder::new(reader));
    for entry in archive.entries()? {
        let mut entry = entry?;
        // Only real files and directories. A tar can name a symlink
        // pointing anywhere on the host, and nothing in a shader
        // collection needs one, so the safe read of an entry we don't
        // understand is to skip it.
        let kind = entry.header().entry_type();
        if !kind.is_file() && !kind.is_dir() {
            continue;
        }
        let path = entry.path()?.into_owned();
        let mut components = path.components();
        components.next(); // the archive's own top-level directory
        let rel = components.as_path();
        if rel.as_os_str().is_empty() {
            continue;
        }
        // `..` or an absolute path in an archive is how a tarball writes
        // outside the directory it is being unpacked into.
        if rel.components().any(|c| !matches!(c, Component::Normal(_))) {
            continue;
        }
        let out = into.join(rel);
        if kind.is_dir() {
            std::fs::create_dir_all(&out)?;
            continue;
        }
        if let Some(parent) = out.parent() {
            std::fs::create_dir_all(parent)?;
        }
        entry.unpack(&out)?;
    }
    Ok(())
}

/// Counts what passes through, so the window can say how far along the
/// download is while it streams into the unpacker.
struct Counting<'a, R> {
    inner: R,
    bytes: &'a AtomicU64,
}

impl<R: Read> Read for Counting<'_, R> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = self.inner.read(buf)?;
        self.bytes.fetch_add(n as u64, Ordering::Relaxed);
        Ok(n)
    }
}
