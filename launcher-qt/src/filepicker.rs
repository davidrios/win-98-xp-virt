//! What is left of the egui build's `filepicker.rs` once Qt is the
//! toolkit: the `Filter` type, and a translation of it into the
//! `nameFilters` string `QtQuick.Dialogs`' `FileDialog` wants.
//!
//! The egui version is 94 lines and pulls in `rfd` — a whole dependency
//! whose job is to reach NSOpenPanel / `IFileDialog` / the XDG portal,
//! because egui draws pixels and has no dialogs. Qt has the same three
//! backends built in (`QFileDialog`/`QtQuick.Dialogs` uses the portal on
//! Linux exactly as `rfd`'s `xdg-portal` feature does), and QML opens it
//! declaratively, so the browsing logic here is `qml/PathField.qml` and
//! this module is a type alias plus a `format!`.
//!
//! Kept as `crate::filepicker` because `disc_library.rs` — one of the
//! nine modules included verbatim from `launcher/src/` — refers to
//! `filepicker::Filter` for its `DISC_FILTER` constant.

/// One extension filter for the dialog, e.g. `("Disk images", &["qcow2"])`.
/// Identical to the egui build's, so the shared constants (`DISC_FILTER`)
/// need no edit.
pub type Filter<'a> = (&'a str, &'a [&'a str]);

pub const DISK_FILTER: Filter<'static> = ("Disk images", &["qcow2", "img", "raw"]);
pub const DISC_FILTER: Filter<'static> = ("Disc images", &["iso", "cue", "ccd", "mds"]);
pub const PRESET_FILTER: Filter<'static> = ("Shader presets", &["slangp"]);
pub const IMAGE_FILTER: Filter<'static> = ("Images", &["png", "jpg", "jpeg", "bmp"]);

/// `("Disc images", ["iso", "cue"])` -> `"Disc images (*.iso *.cue)"`,
/// one entry of QML `FileDialog.nameFilters`. "All files (*)" is always
/// offered alongside it by the caller: a filter that hides the file
/// someone is looking for (a `.bin` beside its `.cue`, an image with an
/// odd extension) is worse than no filter.
pub fn name_filter(filter: Filter) -> String {
    let (name, extensions) = filter;
    let globs: Vec<String> = extensions.iter().map(|e| format!("*.{e}")).collect();
    format!("{name} ({})", globs.join(" "))
}

/// The directory a path field's dialog should open in — the egui build's
/// `start_dir`, unchanged, because the question ("where does Browse…
/// start") is the toolkit's problem only in how the answer is delivered.
pub fn start_dir(value: &str) -> Option<std::path::PathBuf> {
    if value.is_empty() {
        return None;
    }
    let path = std::path::Path::new(value);
    if path.is_dir() {
        return Some(path.to_path_buf());
    }
    path.parent().filter(|p| !p.as_os_str().is_empty()).map(|p| p.to_path_buf())
}
