//! The disc-shelf window (doc 07: "disc shelf editing, one-click
//! guest-tools ISO attach").
//!
//! One window, two modes. Opened from the bottom button row it manages
//! the shared shelf itself (`disc_library.rs`) — add, label, remove.
//! Opened from a machine's row it shows the same shelf plus that
//! machine's two disc-related decisions: which disc is in the drive at
//! **boot** (a bundle edit, `Machine::disc`) and — while the machine is
//! running — which disc to **insert** right now (a monitor command,
//! `control.rs`). The shelf is deliberately not filtered per machine:
//! it's the user's collection, and any disc can go in any drive.
//!
//! Library edits save as they're made; there's no "Save" button, because
//! a shelf is a list of things you own, not a document being drafted.

use crate::bundle::Machine;
use crate::disc_library::{self, DiscLibrary, DISC_FILTER};
use crate::filepicker;
use std::path::{Path, PathBuf};

/// What the window needs when it was opened for a particular machine.
struct MachineContext {
    bundle_path: PathBuf,
    name: String,
    /// The machine's current boot disc, mirrored here so a click updates
    /// the row markers before the library is rescanned.
    boot: Option<PathBuf>,
}

#[derive(Default)]
pub struct DiscShelf {
    open: bool,
    library_path: PathBuf,
    library: DiscLibrary,
    /// The shelf changed this frame and needs writing back. Collected
    /// rather than saved inline so an edited label doesn't write the
    /// file on every keystroke's borrow of the list.
    dirty: bool,
    /// `None` when the window was opened for the shelf itself rather
    /// than for one machine.
    machine: Option<MachineContext>,
    /// The "add a disc by typing a path" field, paired with "Browse…".
    add_path: String,
    /// The result of the last operation (live or otherwise), if any.
    status: Option<String>,
    error: Option<String>,
}

impl DiscShelf {
    /// Open on the shared shelf alone, with no machine context.
    pub fn open_library(&mut self, library_path: &Path) {
        *self = DiscShelf { open: true, ..Default::default() };
        self.load(library_path);
    }

    /// Open for one machine: the same shelf, plus its boot-disc choice
    /// and (while running) live insert.
    pub fn open_for(&mut self, machine: &Machine, bundle_path: PathBuf, library_path: &Path) {
        *self = DiscShelf {
            open: true,
            machine: Some(MachineContext {
                bundle_path,
                name: machine.name.clone(),
                boot: machine.boot_disc().cloned(),
            }),
            ..Default::default()
        };
        self.load(library_path);
    }

    fn load(&mut self, library_path: &Path) {
        self.library_path = library_path.to_path_buf();
        match DiscLibrary::load(library_path) {
            Ok(library) => self.library = library,
            // A corrupt shelf is reported, never silently replaced with
            // an empty one — the next save would then destroy it.
            Err(e) => self.error = Some(format!("{}: {e}", library_path.display())),
        }
    }

    pub fn discs(&self) -> &[disc_library::Disc] {
        &self.library.discs
    }

    /// Write pending shelf edits now. `show` does this at the end of any
    /// frame that changed something; a headless caller (`main.rs`'s
    /// `--discs`) has no frame and calls it directly.
    pub fn flush(&mut self) -> std::io::Result<()> {
        if std::mem::take(&mut self.dirty) {
            self.library.save(&self.library_path)?;
        }
        Ok(())
    }

    /// The directory of the bundle this window has open, so the caller
    /// can tell whether *that* machine is the running one.
    pub fn bundle_dir(&self) -> Option<&Path> {
        self.machine.as_ref().and_then(|m| m.bundle_path.parent())
    }

    /// Renders the window if open. Returns the bundle path when a
    /// machine's boot disc changed, so the caller can rescan the library.
    pub fn show(&mut self, ctx: &egui::Context, running: bool) -> Option<PathBuf> {
        if !self.open {
            return None;
        }
        let mut changed_bundle = None;
        let mut still_open = true;
        let title = match &self.machine {
            Some(m) => format!("Discs — {}", m.name),
            None => "Disc shelf".to_string(),
        };
        egui::Window::new(title)
            .id(egui::Id::new("disc-shelf"))
            .open(&mut still_open)
            .collapsible(false)
            .default_width(680.0)
            // Bounds the truncating path labels below: without a maximum
            // an auto-sizing window just grows to fit the longest path.
            .max_width(820.0)
            .show(ctx, |ui| {
                self.header_ui(ui, running);
                if self.library.discs.is_empty() {
                    ui.label("The shelf is empty.");
                } else {
                    changed_bundle = self.list_ui(ui, running);
                }
                ui.separator();
                self.add_ui(ui);
                if let Some(status) = &self.status {
                    ui.label(status.clone());
                }
                if let Some(err) = &self.error {
                    ui.colored_label(egui::Color32::RED, err);
                }
            });
        self.open = still_open;
        if let Err(e) = self.flush() {
            self.error = Some(e.to_string());
        }
        changed_bundle
    }

    fn header_ui(&mut self, ui: &mut egui::Ui, running: bool) {
        let Some(machine) = &self.machine else {
            ui.label("Discs available to every machine. A machine picks one to boot with; the rest are swapped in while it runs.");
            ui.separator();
            return;
        };
        let boot = machine.boot.clone();
        let mut clear_boot = false;
        ui.horizontal(|ui| {
            let label =
                boot.as_deref().map(disc_library::default_label).unwrap_or_else(|| "(empty tray)".to_string());
            ui.label(format!("Boots with: {label}"));
            clear_boot = ui.add_enabled(boot.is_some(), egui::Button::new("Boot with an empty tray")).clicked();
        });
        if clear_boot {
            self.set_boot(None);
        }
        if running {
            ui.horizontal(|ui| {
                ui.label("Running: “Insert” swaps the disc in the guest now; the boot choice applies next time.");
                if ui.button("Eject").clicked() {
                    self.eject_live();
                }
            });
        }
        ui.separator();
    }

    /// The shelf rows.
    ///
    /// Buttons come *before* the path: a disc image's path is routinely
    /// long enough to widen a grid column past the screen, and a column
    /// after it would go with it. The path column truncates from the
    /// right, which is why the label (editable, defaulting to the file
    /// name) is its own column — truncating a full path leaves every row
    /// reading `/home/…/…/…` identically. The full path is on hover.
    fn list_ui(&mut self, ui: &mut egui::Ui, running: bool) -> Option<PathBuf> {
        let mut remove = None;
        let mut insert = None;
        let mut set_boot = None;
        let for_machine = self.machine.is_some();
        let boot = self.machine.as_ref().and_then(|m| m.boot.clone());
        let mut dirty = false;
        egui::Grid::new("disc-shelf-grid").striped(true).num_columns(4).show(ui, |ui| {
            for disc in &mut self.library.discs {
                ui.horizontal(|ui| {
                    if running && ui.button("Insert").clicked() {
                        insert = Some(disc.path.clone());
                    }
                    if for_machine {
                        let is_boot = boot.as_deref() == Some(disc.path.as_path());
                        if ui
                            .selectable_label(is_boot, "Boot")
                            .on_hover_text("Put this disc in the drive when the machine starts")
                            .clicked()
                        {
                            set_boot = Some(disc.path.clone());
                        }
                    }
                    if ui.button("Remove").clicked() {
                        remove = Some(disc.path.clone());
                    }
                });
                let full = disc.path.display().to_string();
                // `add_sized`, not `desired_width`: a grid column takes
                // the width its cells actually claim, and a bare
                // TextEdit in one claims almost nothing.
                let label_size = egui::vec2(190.0, ui.spacing().interact_size.y);
                if ui.add_sized(label_size, egui::TextEdit::singleline(&mut disc.label)).changed() {
                    dirty = true;
                }
                let name =
                    disc.path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| full.clone());
                ui.label(&name).on_hover_text(&full);
                let dir = disc.path.parent().map(|p| p.display().to_string()).unwrap_or_default();
                ui.add(egui::Label::new(egui::RichText::new(&dir).weak()).truncate()).on_hover_text(&full);
                ui.end_row();
            }
        });
        self.dirty |= dirty;
        if let Some(path) = remove {
            self.remove(&path);
        }
        if let Some(path) = insert {
            self.insert_live(&path);
        }
        set_boot.and_then(|path| self.set_boot(Some(path)))
    }

    fn add_ui(&mut self, ui: &mut egui::Ui) {
        filepicker::path_field(ui, "Add disc", &mut self.add_path, Some(DISC_FILTER));
        ui.horizontal(|ui| {
            let can_add = !self.add_path.trim().is_empty();
            if ui.add_enabled(can_add, egui::Button::new("Add to shelf")).clicked() {
                let path = PathBuf::from(self.add_path.trim());
                self.add(path);
                self.add_path.clear();
            }
            // Doc 07's one-click guest-tools attach: no path to find, no
            // browsing — the driver/test ISO this checkout last built.
            match disc_library::guest_tools_iso() {
                Some(iso) => {
                    if ui.button("Add guest-tools ISO").on_hover_text(iso.display().to_string()).clicked() {
                        self.add(iso);
                    }
                }
                None => {
                    ui.add_enabled(false, egui::Button::new("Add guest-tools ISO"))
                        .on_disabled_hover_text("none built (guest-tools/build-wrappers.sh)");
                }
            }
        });
    }

    /// Put a disc on the shelf. Public so `main.rs`'s `--discs` debug
    /// verb runs the same call the button does.
    pub fn add(&mut self, path: PathBuf) {
        let label = disc_library::default_label(&path);
        if self.library.add(path) {
            self.dirty = true;
            self.status = Some(format!("added {label}"));
            self.error = None;
        } else {
            self.status = Some(format!("{label} is already on the shelf"));
        }
    }

    pub fn remove(&mut self, path: &Path) {
        if let Some(i) = self.library.position(path) {
            let disc = self.library.discs.remove(i);
            self.dirty = true;
            self.status = Some(format!("removed {}", disc.label));
            self.error = None;
        }
    }

    /// Set (or clear) the open machine's boot disc, writing the bundle.
    /// Returns the bundle path on success so the grid can rescan.
    pub fn set_boot(&mut self, path: Option<PathBuf>) -> Option<PathBuf> {
        let machine = self.machine.as_mut()?;
        // Re-read rather than editing a `Machine` captured when the
        // window opened: the wizard may have saved the same bundle in
        // between, and only the boot disc is this window's to change.
        let result = Machine::load(&machine.bundle_path).and_then(|mut m| {
            m.disc = path.clone();
            m.discs.clear();
            m.save(&machine.bundle_path)?;
            Ok(())
        });
        match result {
            Ok(()) => {
                machine.boot = path.clone();
                self.status = Some(match &path {
                    Some(p) => format!("boots with {}", disc_library::default_label(p)),
                    None => "boots with an empty tray".to_string(),
                });
                self.error = None;
                Some(self.machine.as_ref()?.bundle_path.clone())
            }
            Err(e) => {
                self.error = Some(e.to_string());
                None
            }
        }
    }

    /// Swap `disc` into the running machine's drive. Public so
    /// `main.rs`'s `--insert-disc` debug verb runs the same call the
    /// "Insert" button does.
    pub fn insert_live(&mut self, disc: &Path) {
        let name = disc.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let disc = disc.to_path_buf();
        self.live(|c| c.insert_disc(&disc), &format!("inserted {name}"));
    }

    pub fn eject_live(&mut self) {
        self.live(|c| c.eject_disc(), "ejected");
    }

    /// The last operation's result: the status line, or the error.
    pub fn last_result(&self) -> Result<Option<&str>, &str> {
        match &self.error {
            Some(e) => Err(e),
            None => Ok(self.status.as_deref()),
        }
    }

    /// Run one operation on the running machine's monitor. A fresh
    /// connection each time (see `snapshots::SnapshotWindow::control`);
    /// failures land in the window's error line rather than a panic —
    /// the guest may have shut down between the repaint that drew the
    /// button and the click on it.
    fn live(&mut self, op: impl FnOnce(&mut crate::control::Control) -> Result<(), String>, done: &str) {
        let Some(dir) = self.bundle_dir() else {
            self.error = Some("no machine open".into());
            return;
        };
        let result = crate::control::Control::connect(&crate::control::socket_path(dir)).and_then(|mut c| op(&mut c));
        match result {
            Ok(()) => {
                self.status = Some(done.to_string());
                self.error = None;
            }
            Err(e) => {
                self.status = None;
                self.error = Some(e);
            }
        }
    }
}
