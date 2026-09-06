//! The disc-shelf window, in egui: widgets over
//! `launcher_core::shelf::Shelf`, which owns the shelf itself, the
//! machine context, the boot-disc write and the live insert/eject.
//!
//! The one thing the view still owns is *when* the shelf is written: an
//! immediate-mode label field would save the file on every keystroke's
//! borrow of the list, so the rows are edited in place, `mark_dirty` is
//! set, and `flush_reporting` runs at the end of the frame. (Qt's
//! `TextField` has an `editingFinished`, so that build calls
//! `set_label` and flushes once — same two methods.)

use crate::filepicker;
use launcher_core::disc_library::{self, DISC_FILTER};
use launcher_core::shelf::Shelf;
use std::path::PathBuf;

#[derive(Default)]
pub struct DiscShelfWindow {
    pub shelf: Shelf,
    /// The "add a disc by typing a path" field, paired with "Browse…" —
    /// a text buffer the window owns while it is being typed into, which
    /// is why it is not in the model.
    add_path: String,
}

impl DiscShelfWindow {
    /// Renders the window if open. Returns the bundle path when a
    /// machine's boot disc changed, so the caller can rescan the library.
    pub fn show(&mut self, ctx: &egui::Context, running: bool) -> Option<PathBuf> {
        if !self.shelf.open {
            return None;
        }
        let mut changed_bundle = None;
        let mut still_open = true;
        egui::Window::new(self.shelf.title())
            .id(egui::Id::new("disc-shelf"))
            .open(&mut still_open)
            .collapsible(false)
            .default_width(680.0)
            // Bounds the truncating path labels below: without a maximum
            // an auto-sizing window just grows to fit the longest path.
            .max_width(820.0)
            .show(ctx, |ui| {
                self.header_ui(ui, running);
                if self.shelf.discs().is_empty() {
                    ui.label("The shelf is empty.");
                } else {
                    changed_bundle = self.list_ui(ui, running);
                }
                ui.separator();
                self.add_ui(ui);
                if let Some(status) = self.shelf.status() {
                    ui.label(status.to_string());
                }
                if let Some(err) = self.shelf.error() {
                    ui.colored_label(egui::Color32::RED, err.to_string());
                }
            });
        self.shelf.open = still_open;
        self.shelf.flush_reporting();
        changed_bundle
    }

    fn header_ui(&mut self, ui: &mut egui::Ui, running: bool) {
        if !self.shelf.for_machine() {
            ui.label("Discs available to every machine. A machine picks one to boot with; the rest are swapped in while it runs.");
            ui.separator();
            return;
        }
        let has_boot = self.shelf.boot().is_some();
        let label = self.shelf.boot_label();
        let mut clear_boot = false;
        ui.horizontal(|ui| {
            ui.label(format!("Boots with: {label}"));
            clear_boot = ui.add_enabled(has_boot, egui::Button::new("Boot with an empty tray")).clicked();
        });
        if clear_boot {
            self.shelf.set_boot(None);
        }
        if running {
            ui.horizontal(|ui| {
                ui.label("Running: “Insert” swaps the disc in the guest now; the boot choice applies next time.");
                if ui.button("Eject").clicked() {
                    self.shelf.eject_live();
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
        let for_machine = self.shelf.for_machine();
        let boot = self.shelf.boot().map(|p| p.to_path_buf());
        let mut dirty = false;
        egui::Grid::new("disc-shelf-grid").striped(true).num_columns(4).show(ui, |ui| {
            for (row, disc) in self.shelf.discs_mut().iter_mut().enumerate() {
                ui.horizontal(|ui| {
                    if running && ui.button("Insert").clicked() {
                        insert = Some(row);
                    }
                    if for_machine {
                        let is_boot = boot.as_deref() == Some(disc.path.as_path());
                        if ui
                            .selectable_label(is_boot, "Boot")
                            .on_hover_text("Put this disc in the drive when the machine starts")
                            .clicked()
                        {
                            set_boot = Some(row);
                        }
                    }
                    if ui.button("Remove").clicked() {
                        remove = Some(row);
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
        if dirty {
            self.shelf.mark_dirty();
        }
        if let Some(row) = remove {
            self.shelf.remove_row(row);
        }
        if let Some(row) = insert {
            self.shelf.insert_live_row(row);
        }
        set_boot.and_then(|row| self.shelf.set_boot_row(row))
    }

    fn add_ui(&mut self, ui: &mut egui::Ui) {
        filepicker::path_field(ui, "Add disc", &mut self.add_path, Some(DISC_FILTER));
        ui.horizontal(|ui| {
            let can_add = !self.add_path.trim().is_empty();
            if ui.add_enabled(can_add, egui::Button::new("Add to shelf")).clicked() {
                self.shelf.add(PathBuf::from(self.add_path.trim()));
                self.add_path.clear();
            }
            // A folder is a disc as well (M5g): `isodir` generates a
            // volume over it as the guest reads it, which is how a pile
            // of files reaches a machine whose networking nobody wants to
            // trust. No file filter can express "a folder", so it is its
            // own dialog and its own button rather than a mode of the
            // field above.
            if ui
                .button("Add folder…")
                .on_hover_text("share a host directory with the guest as a generated disc")
                .clicked()
            {
                if let Some(dir) = filepicker::pick_folder_headless(filepicker::start_dir(&self.add_path).as_deref()) {
                    self.shelf.add(dir);
                }
            }
            // Doc 07's one-click guest-tools attach: no path to find, no
            // browsing — the driver/test ISO this checkout last built.
            match disc_library::guest_tools_iso() {
                Some(iso) => {
                    if ui.button("Add guest-tools ISO").on_hover_text(iso.display().to_string()).clicked() {
                        self.shelf.add_guest_tools();
                    }
                }
                None => {
                    ui.add_enabled(false, egui::Button::new("Add guest-tools ISO"))
                        .on_disabled_hover_text("none built (guest-tools/build-wrappers.sh)");
                }
            }
        });
    }
}
