//! A native file-picker field: egui has no OS file dialog of its own (it
//! draws pixels, nothing else), so this pairs a text field with a
//! "Browse…" button that pops the real system dialog — NSOpenPanel on
//! macOS, the Win32 `IFileDialog` on Windows, the XDG desktop portal
//! (over D-Bus) on Linux/BSD, all through `rfd`. `default-features =
//! false` + the `xdg-portal` feature (see `Cargo.toml`) keeps the Linux
//! build off GTK entirely; the portal backend needs a running
//! `xdg-desktop-portal` service, standard on modern desktops.
//!
//! The dialog call blocks the calling thread. That's fine here: this
//! launcher has no async runtime for it to stall (unlike a GUI built on
//! tokio, where the blocking portal call must stay off the runtime's
//! threads) — it's a plain synchronous egui app on winit's own thread,
//! and a brief block while a modal file dialog is open is expected.

/// One extension filter for the dialog (e.g. `("Disk images", &["qcow2"])`).
pub type Filter<'a> = (&'a str, &'a [&'a str]);

/// Pop the dialog without an egui field around it — a debug verb (`main.rs`'s
/// `--pick-file`) exercises the actual OS integration (portal/NSOpenPanel/
/// IFileDialog) headlessly, since GUI click automation can't drive a real
/// dialog to prove this wiring works.
pub fn pick_file_headless(filter: Option<Filter>) -> Option<std::path::PathBuf> {
    let mut dialog = rfd::FileDialog::new();
    if let Some((name, extensions)) = filter {
        dialog = dialog.add_filter(name, extensions);
    }
    dialog.pick_file()
}

/// A labeled text field with a "Browse…" button. Typing directly is still
/// allowed (a path the user already knows, or one on a mount the picker
/// can't reach); the button is a convenience, not the only way in.
pub fn path_field(ui: &mut egui::Ui, label: &str, value: &mut String, filter: Option<Filter>) {
    ui.horizontal(|ui| {
        ui.label(label);
        ui.text_edit_singleline(value);
        if ui.button("Browse…").clicked() {
            if let Some(path) = pick_file_headless(filter) {
                *value = path.display().to_string();
            }
        }
    });
}
