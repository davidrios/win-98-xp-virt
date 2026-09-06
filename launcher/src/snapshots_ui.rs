//! The per-machine snapshot window, in egui: widgets over
//! `launcher_core::snaps::Snapshots`, which owns the state machine —
//! which source the list comes from, the in-flight job and its polling,
//! and every operation the buttons run.
//!
//! Two things stay here because they are this toolkit's answer, not the
//! product's: the poll happens at the top of a frame (there is a frame
//! anyway; Qt runs a `Timer` that stops when there is no job), and a
//! destructive restore is confirmed by the row's button turning into
//! "Discard current state?" for one more click, rather than by a modal.

use launcher_core::snaps::Snapshots;

#[derive(Default)]
pub struct SnapshotWindow {
    pub model: Snapshots,
    /// The "New snapshot" name being typed.
    new_name: String,
    /// A pending "Restore" waiting for its confirmation click. Restoring
    /// overwrites the disk's current state with the snapshot's, and
    /// there is no undo — a stray click on a row shouldn't do that.
    confirm_restore: Option<String>,
}

impl SnapshotWindow {
    /// Renders the window if open. `running` says whether *this*
    /// machine's player is up: if it is, every operation goes through
    /// the machine's monitor, because `qemu-img` writing to an image
    /// QEMU has open corrupts it and even the listing wants an image
    /// lock QEMU already holds.
    pub fn show(&mut self, ctx: &egui::Context, running: bool) {
        if !self.model.open {
            return;
        }
        self.model.set_running(running);
        self.model.poll_job();
        let mut still_open = true;
        egui::Window::new(self.model.title())
            .id(egui::Id::new("snapshots"))
            .open(&mut still_open)
            .collapsible(false)
            .default_width(600.0)
            .max_width(760.0)
            .show(ctx, |ui| {
                if self.model.running() {
                    ui.label("Live: this machine is running, so a snapshot also stores its RAM and CPU state.");
                    ui.separator();
                }
                if self.model.snapshots().is_empty() {
                    ui.label("No snapshots.");
                } else {
                    self.list_ui(ui);
                }
                ui.separator();
                ui.horizontal(|ui| {
                    ui.label("New snapshot");
                    ui.text_edit_singleline(&mut self.new_name);
                    let name = self.new_name.trim().to_string();
                    // A job in flight owns the guest's state; a second
                    // one on top of it is refused by QEMU anyway.
                    let ready = !name.is_empty() && !self.model.job_pending();
                    if ui.add_enabled(ready, egui::Button::new("Take snapshot")).clicked() {
                        self.model.take(&name);
                        self.new_name.clear();
                    }
                });
                if let Some(status) = self.model.status() {
                    ui.label(status.to_string());
                }
                if let Some(err) = self.model.error() {
                    ui.colored_label(egui::Color32::RED, err.to_string());
                }
            });
        self.model.open = still_open;
    }

    fn list_ui(&mut self, ui: &mut egui::Ui) {
        let mut restore_name = None;
        let mut delete_name = None;
        // Held in a local so the row loop can borrow the list while
        // still arming the confirmation, and applied after the loop —
        // the list is what a restore/delete replaces.
        let mut confirm = self.confirm_restore.clone();
        let idle = !self.model.job_pending();
        let list = self.model.snapshots();
        egui::Grid::new("snapshot-grid").striped(true).num_columns(4).show(ui, |ui| {
            ui.strong("Name");
            ui.strong("Taken");
            ui.strong("VM state");
            ui.strong("");
            ui.end_row();
            for snap in list {
                // Not a truncating label: a leading grid column given
                // `truncate()` collapses to a few characters ("clea…"),
                // and unlike a disc path a snapshot tag is short.
                ui.label(&snap.name);
                ui.label(snap.date_label());
                ui.label(snap.size_label());
                ui.horizontal(|ui| {
                    if confirm.as_deref() == Some(snap.name.as_str()) {
                        if ui.add_enabled(idle, egui::Button::new("Discard current state?")).clicked() {
                            restore_name = Some(snap.name.clone());
                        }
                    } else if ui.add_enabled(idle, egui::Button::new("Restore")).clicked() {
                        confirm = Some(snap.name.clone());
                    }
                    if ui.add_enabled(idle, egui::Button::new("Delete")).clicked() {
                        delete_name = Some(snap.name.clone());
                    }
                });
                ui.end_row();
            }
        });
        self.confirm_restore = confirm;
        if let Some(name) = restore_name {
            self.confirm_restore = None;
            self.model.revert(&name);
        }
        if let Some(name) = delete_name {
            self.confirm_restore = None;
            self.model.drop_snapshot(&name);
        }
    }
}
