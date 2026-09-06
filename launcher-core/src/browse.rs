//! The part of "Browse…" that is not a dialog.
//!
//! Neither front end's file dialog is here: the egui build has to bring
//! one (`rfd` — egui draws pixels and nothing else), Qt ships its own
//! (`QtQuick.Dialogs`' `FileDialog`), and both end up on the same three
//! backends anyway — the XDG portal on Linux, `NSOpenPanel` on macOS,
//! `IFileDialog` on Windows. What *is* here is the decision they were
//! separately getting right: which extensions a field offers, and which
//! directory the dialog opens in. A `.slangp` lives somewhere nobody
//! would navigate to by hand, so getting that wrong is the difference
//! between a working button and a dialog on the user's home directory.

use std::path::{Path, PathBuf};

/// One extension filter for a dialog (e.g. `("Disk images", &["qcow2"])`).
/// A plain pair rather than a toolkit type: each front end turns it into
/// whatever its own dialog wants — `rfd::FileDialog::add_filter` here,
/// a `"Disk images (*.qcow2)"` string for Qt's `nameFilters` there.
pub type Filter<'a> = (&'a str, &'a [&'a str]);

/// A Qt-style `"Disk images (*.qcow2 *.img)"` name filter. Qt's
/// `FileDialog` takes those, so the same constants drive both dialogs
/// instead of the QML repeating the extension lists by hand.
pub fn name_filter(filter: Filter) -> String {
    let (label, extensions) = filter;
    let globs: Vec<String> = extensions.iter().map(|e| format!("*.{e}")).collect();
    format!("{label} ({})", globs.join(" "))
}

/// The directory a path field's "Browse…" should open in: the value's own
/// directory if it names one (a file inside it, or the directory itself),
/// `None` (the OS default — the platform picker's own last-used location,
/// or an initial default) if the field is empty or names a bare filename.
///
/// A first attempt handed the dialog the *file* path instead of the
/// directory containing it, which breaks it; `cli`'s `--pick-file` verb
/// exercises this exact function for that reason.
pub fn start_dir(value: &str) -> Option<PathBuf> {
    if value.is_empty() {
        return None;
    }
    let path = Path::new(value);
    if path.is_dir() {
        return Some(path.to_path_buf());
    }
    path.parent().filter(|p| !p.as_os_str().is_empty()).map(|p| p.to_path_buf())
}

/// Where "Browse…" actually opens: the field's own value if it points
/// somewhere (`start_dir`), else the caller's suggestion for an empty
/// field (the preset collection, for the shader editor's preset field),
/// else the OS default. Its own function so `cli`'s `--browse-start`
/// verb can check the choice without popping a modal dialog only a human
/// could answer.
pub fn browse_start(value: &str, empty_dir: Option<&Path>) -> Option<PathBuf> {
    start_dir(value).or_else(|| empty_dir.map(|d| d.to_path_buf()))
}
