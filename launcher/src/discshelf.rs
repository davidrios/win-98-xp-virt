//! Disc-shelf editing (doc 07: "Snapshots UI, disc shelf editing,
//! one-click guest-tools ISO attach"). The shelf is `Machine::discs` — the
//! ordered list of disc images a machine owns, of which the first is the
//! one `qemu_args` attaches as the boot-time CD-ROM. Multi-disc installs
//! are the reason it's a list and not a single path: the wizard's single
//! "install media" slot can only ever describe disc 1.
//!
//! Editing here is a *bundle* edit, so it applies to the next boot. It
//! stays available while a machine is running (nothing it writes can
//! affect a live guest), and the window says so rather than greying
//! itself out.

use crate::bundle::Machine;
use crate::filepicker;
use std::path::PathBuf;

const DISC_FILTER: filepicker::Filter = ("Disc images", &["iso", "cue", "ccd", "mds"]);

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
    // Canonicalized because this one goes *into* a bundle file: the
    // build-time anchor is `<manifest>/../guest-tools/out`, and a
    // `launcher/../guest-tools` in a saved `machine.toml` would be
    // correct but unreadable.
    candidates.pop().map(|(_, path)| path.canonicalize().unwrap_or(path))
}

#[derive(Default)]
pub struct DiscShelf {
    open: bool,
    /// The bundle being edited; `None` when the window has never opened.
    bundle_path: Option<PathBuf>,
    machine_name: String,
    discs: Vec<PathBuf>,
    /// The "add a disc by typing a path" field, paired with "Browse…".
    add_path: String,
    error: Option<String>,
}

impl DiscShelf {
    pub fn open_for(&mut self, machine: &Machine, bundle_path: PathBuf) {
        *self = DiscShelf {
            open: true,
            bundle_path: Some(bundle_path),
            machine_name: machine.name.clone(),
            discs: machine.discs.clone(),
            ..Default::default()
        };
    }

    /// Headless equivalent of the window's buttons, for `main.rs`'s
    /// `--disc-shelf` debug verb: the same `save()` a click on "Save"
    /// runs, over fields the widgets would otherwise have set.
    pub fn set_discs(&mut self, discs: Vec<PathBuf>) {
        self.discs = discs;
    }

    pub fn discs(&self) -> &[PathBuf] {
        &self.discs
    }

    /// The directory of the bundle this window has open, so the caller
    /// can tell whether *that* machine is the running one.
    pub fn bundle_dir(&self) -> Option<&std::path::Path> {
        self.bundle_path.as_deref().and_then(|p| p.parent())
    }

    /// Renders the window if open. Returns the bundle path once the shelf
    /// has been written, so the caller can rescan the library.
    pub fn show(&mut self, ctx: &egui::Context, running: bool) -> Option<PathBuf> {
        if !self.open {
            return None;
        }
        let mut done = None;
        let mut still_open = true;
        egui::Window::new(format!("Discs — {}", self.machine_name))
            .id(egui::Id::new("disc-shelf"))
            .open(&mut still_open)
            .collapsible(false)
            .default_width(640.0)
            // Bounds the truncating path labels below: without a maximum
            // an auto-sizing window just grows to fit the longest path.
            .max_width(760.0)
            .show(ctx, |ui| {
                if running {
                    ui.label("This machine is running: shelf edits apply to its next boot.");
                    ui.separator();
                }
                if self.discs.is_empty() {
                    ui.label("No discs on the shelf.");
                } else {
                    self.list_ui(ui);
                }
                ui.separator();
                filepicker::path_field(ui, "Add disc", &mut self.add_path, Some(DISC_FILTER));
                ui.horizontal(|ui| {
                    let can_add = !self.add_path.trim().is_empty();
                    if ui.add_enabled(can_add, egui::Button::new("Add to shelf")).clicked() {
                        self.discs.push(PathBuf::from(self.add_path.trim()));
                        self.add_path.clear();
                    }
                    // Doc 07's one-click guest-tools attach: no path to
                    // find, no browsing — the driver/test ISO this
                    // checkout last built.
                    match guest_tools_iso() {
                        Some(iso) => {
                            if ui.button("Add guest-tools ISO").on_hover_text(iso.display().to_string()).clicked() {
                                self.discs.push(iso);
                            }
                        }
                        None => {
                            ui.add_enabled(false, egui::Button::new("Add guest-tools ISO"))
                                .on_disabled_hover_text("none built (guest-tools/build-wrappers.sh)");
                        }
                    }
                });
                if let Some(err) = &self.error {
                    ui.colored_label(egui::Color32::RED, err);
                }
                ui.separator();
                if ui.button("Save").clicked() {
                    match self.save() {
                        Ok(path) => {
                            done = Some(path);
                            self.error = None;
                        }
                        Err(e) => self.error = Some(e.to_string()),
                    }
                }
            });
        self.open = still_open && done.is_none();
        done
    }

    /// The shelf rows. Reorder/remove are applied after the loop so the
    /// list isn't mutated while it's being iterated for layout.
    ///
    /// Buttons come *before* the path: a disc image's path is routinely
    /// long enough to widen a grid column past the screen, and a column
    /// after it would go with it. The path is then split file name /
    /// directory, because egui truncates from the *right* and the file
    /// name is the half that identifies a disc — truncating the whole
    /// path leaves every row reading `/home/…/…/…` identically. The full
    /// path is on hover either way.
    fn list_ui(&mut self, ui: &mut egui::Ui) {
        let mut swap = None;
        let mut remove = None;
        egui::Grid::new("disc-shelf-grid").striped(true).num_columns(4).show(ui, |ui| {
            for (i, disc) in self.discs.iter().enumerate() {
                ui.horizontal(|ui| {
                    if ui.add_enabled(i > 0, egui::Button::new("Up")).clicked() {
                        swap = Some((i - 1, i));
                    }
                    if ui.add_enabled(i + 1 < self.discs.len(), egui::Button::new("Down")).clicked() {
                        swap = Some((i, i + 1));
                    }
                    if ui.button("Remove").clicked() {
                        remove = Some(i);
                    }
                });
                // The first entry is what `Machine::qemu_args` attaches as
                // the CD-ROM; the rest exist to be swapped in.
                ui.label(if i == 0 { "boot" } else { "" });
                let full = disc.display().to_string();
                let name = disc.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| full.clone());
                let dir = disc.parent().map(|p| p.display().to_string()).unwrap_or_default();
                ui.label(&name).on_hover_text(&full);
                ui.add(egui::Label::new(egui::RichText::new(&dir).weak()).truncate()).on_hover_text(&full);
                ui.end_row();
            }
        });
        if let Some((a, b)) = swap {
            self.discs.swap(a, b);
        }
        if let Some(i) = remove {
            self.discs.remove(i);
        }
    }

    /// Write the shelf back into the bundle. Everything else in the file
    /// is preserved by re-reading it here rather than editing a `Machine`
    /// captured when the window opened: the wizard may have saved the
    /// same bundle in between, and only `discs` is this window's to change.
    pub fn save(&self) -> std::io::Result<PathBuf> {
        let path = self.bundle_path.clone().ok_or_else(|| std::io::Error::other("no bundle open"))?;
        let mut machine = Machine::load(&path)?;
        machine.discs = self.discs.clone();
        machine.save(&path)?;
        Ok(path)
    }
}
