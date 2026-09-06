//! A native file-picker field: egui has no OS file dialog of its own (it
//! draws pixels, nothing else), so this pairs a text field with a
//! "Browse…" button that pops the real system dialog — NSOpenPanel on
//! macOS, the Win32 `IFileDialog` on Windows, the XDG desktop portal
//! (over D-Bus) on Linux/BSD, all through `rfd`. `default-features =
//! false` + the `xdg-portal` feature (see `Cargo.toml`) keeps the Linux
//! build off GTK entirely; the portal backend needs a running
//! `xdg-desktop-portal` service, standard on modern desktops.
//!
//! This is one of the two files that exist *because* of the toolkit
//! (the Qt front end has none: `QtQuick.Dialogs`' `FileDialog` reaches
//! the same three backends declaratively). What is *not* here is which
//! extensions a field offers and where the dialog opens — those are
//! decisions, not widgets, and they live in `launcher_core::browse` so
//! both front ends make them the same way.
//!
//! The dialog call blocks the calling thread. That's fine here: this
//! launcher has no async runtime for it to stall (unlike a GUI built on
//! tokio, where the blocking portal call must stay off the runtime's
//! threads) — it's a plain synchronous egui app on winit's own thread,
//! and a brief block while a modal file dialog is open is expected.

pub use launcher_core::browse::{browse_start, start_dir, Filter};

/// Pop the dialog without an egui field around it — `main.rs`'s
/// `--pick-file` exercises the actual OS integration headlessly, since
/// GUI click automation can't drive a real dialog to prove this wiring
/// works. `start_dir`, when given, is where the dialog opens.
pub fn pick_file_headless(filter: Option<Filter>, start_dir: Option<&std::path::Path>) -> Option<std::path::PathBuf> {
    let mut dialog = rfd::FileDialog::new();
    if let Some((name, extensions)) = filter {
        dialog = dialog.add_filter(name, extensions);
    }
    if let Some(dir) = start_dir {
        dialog = dialog.set_directory(dir);
    }
    dialog.pick_file()
}

/// The same for a *directory*: a shared folder goes on the shelf as a
/// disc of its own (`disc_library::qemu_medium`), and no file filter can
/// express "a folder", so the dialog has to be a different one.
pub fn pick_folder_headless(start_dir: Option<&std::path::Path>) -> Option<std::path::PathBuf> {
    let mut dialog = rfd::FileDialog::new();
    if let Some(dir) = start_dir {
        dialog = dialog.set_directory(dir);
    }
    dialog.pick_folder()
}

/// A labeled text field with a "Browse…" button. Typing directly is still
/// allowed (a path the user already knows, or one on a mount the picker
/// can't reach); the button is a convenience, not the only way in.
pub fn path_field(ui: &mut egui::Ui, label: &str, value: &mut String, filter: Option<Filter>) {
    path_field_in(ui, label, value, filter, None);
}

/// The same, with somewhere for the dialog to open when the field is
/// still empty — the shader preset field points it at the preset
/// collection, which is otherwise buried in a data directory nobody
/// would navigate to by hand. A field that already has a value still
/// wins.
pub fn path_field_in(
    ui: &mut egui::Ui,
    label: &str,
    value: &mut String,
    filter: Option<Filter>,
    empty_dir: Option<&std::path::Path>,
) {
    ui.horizontal(|ui| {
        ui.label(label);
        ui.text_edit_singleline(value);
        if ui.button("Browse…").clicked() {
            let start = browse_start(value, empty_dir);
            if let Some(path) = pick_file_headless(filter, start.as_deref()) {
                *value = path.display().to_string();
            }
        }
    });
}
