//! Snapshots (doc 07: "QEMU internal snapshots via in-proc QMP, surfaced
//! in the overlay and the launcher").
//!
//! This is the *offline* half — a machine that isn't running has no
//! monitor to ask, so the launcher goes at the qcow2 directly with
//! `qemu-img`, which is what QEMU's own `savevm`/`loadvm` write into.
//! Listing goes through `qemu-img info --output=json` rather than
//! `snapshot -l`'s column layout: the JSON is a stable interface, the
//! table is formatted for humans and has no escaping for a tag with a
//! space in it.
//!
//! Restoring is `qemu-img snapshot -a`, which rolls the *disk* back and
//! leaves the saved CPU/RAM state in the image for a later `loadvm` — the
//! same thing a cold boot into a snapshot means. Reverting a running
//! machine is the live half (`control.rs`).

use crate::player;
use std::path::Path;
use std::process::Command;

#[derive(Debug, Clone)]
pub struct Snapshot {
    pub id: String,
    pub name: String,
    /// Size of the saved CPU/RAM state, 0 for a disk-only snapshot
    /// (`qemu-img snapshot -c` makes those; `savevm` makes the other kind).
    pub vm_state_size: u64,
    /// Unix seconds when it was taken, as qcow2 records it.
    pub date_sec: u64,
}

impl Snapshot {
    /// `2026-09-05 14:03` in local time, or the raw seconds if that can't
    /// be formed — this is a label in a list, never a parsed value.
    pub fn date_label(&self) -> String {
        let secs = self.date_sec as i64;
        // No chrono/time dependency for one label: civil-from-days
        // (Howard Hinnant's algorithm), UTC. A snapshot list sorted by a
        // timestamp that's an hour off in the user's head is not worth a
        // timezone database.
        let days = secs.div_euclid(86_400);
        let rem = secs.rem_euclid(86_400);
        let z = days + 719_468;
        let era = z.div_euclid(146_097);
        let doe = z.rem_euclid(146_097);
        let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
        let y = yoe + era * 400;
        let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        let mp = (5 * doy + 2) / 153;
        let d = doy - (153 * mp + 2) / 5 + 1;
        let m = if mp < 10 { mp + 3 } else { mp - 9 };
        let y = if m <= 2 { y + 1 } else { y };
        format!("{y:04}-{m:02}-{d:02} {:02}:{:02} UTC", rem / 3600, (rem % 3600) / 60)
    }

    /// "12.4 MB" / "—" for a disk-only snapshot.
    pub fn size_label(&self) -> String {
        if self.vm_state_size == 0 {
            return "—".into();
        }
        let mb = self.vm_state_size as f64 / (1024.0 * 1024.0);
        if mb >= 1024.0 {
            format!("{:.1} GB", mb / 1024.0)
        } else {
            format!("{mb:.1} MB")
        }
    }
}

fn qemu_img(args: &[&str], disk: &Path) -> std::io::Result<std::process::Output> {
    let bin = player::qemu_img_binary();
    Command::new(&bin)
        .args(args)
        .arg(disk)
        .output()
        .map_err(|e| std::io::Error::other(format!("running {}: {e}", bin.display())))
}

/// Fail with `qemu-img`'s own stderr rather than a bare exit code — its
/// messages ("Could not find snapshot 'x'", "Permission denied") are
/// exactly what the window should show.
fn check(what: &str, out: &std::process::Output) -> std::io::Result<()> {
    if out.status.success() {
        return Ok(());
    }
    let err = String::from_utf8_lossy(&out.stderr);
    let err = err.trim();
    Err(std::io::Error::other(if err.is_empty() {
        format!("qemu-img {what}: exited with {}", out.status)
    } else {
        format!("qemu-img {what}: {err}")
    }))
}

/// Every internal snapshot in `disk`, newest last (qcow2 order). An image
/// with no snapshot table has none — not an error.
pub fn list(disk: &Path) -> std::io::Result<Vec<Snapshot>> {
    let out = qemu_img(&["info", "--output=json"], disk)?;
    check("info", &out)?;
    let info: serde_json::Value = serde_json::from_slice(&out.stdout).map_err(std::io::Error::other)?;
    let Some(list) = info.get("snapshots").and_then(|s| s.as_array()) else {
        return Ok(Vec::new());
    };
    Ok(list
        .iter()
        .map(|s| Snapshot {
            id: s["id"].as_str().unwrap_or_default().to_string(),
            name: s["name"].as_str().unwrap_or_default().to_string(),
            vm_state_size: s["vm-state-size"].as_u64().unwrap_or(0),
            date_sec: s["date-sec"].as_u64().unwrap_or(0),
        })
        .collect())
}

pub fn create(disk: &Path, name: &str) -> std::io::Result<()> {
    check("snapshot -c", &qemu_img(&["snapshot", "-c", name], disk)?)
}

pub fn delete(disk: &Path, name: &str) -> std::io::Result<()> {
    check("snapshot -d", &qemu_img(&["snapshot", "-d", name], disk)?)
}

pub fn restore(disk: &Path, name: &str) -> std::io::Result<()> {
    check("snapshot -a", &qemu_img(&["snapshot", "-a", name], disk)?)
}

/// The per-machine snapshot window off the library grid.
#[derive(Default)]
pub struct SnapshotWindow {
    open: bool,
    /// The bundle directory, so the caller can say whether *this*
    /// machine is the running one.
    bundle_dir: Option<std::path::PathBuf>,
    machine_name: String,
    disk: std::path::PathBuf,
    list: Vec<Snapshot>,
    new_name: String,
    /// A pending "Restore" waiting for its confirmation click. Restoring
    /// overwrites the disk's current state with the snapshot's, and
    /// there's no undo — a stray click on a row shouldn't do that.
    confirm_restore: Option<String>,
    error: Option<String>,
    status: Option<String>,
}

impl SnapshotWindow {
    pub fn open_for(&mut self, machine: &crate::bundle::Machine, bundle_dir: std::path::PathBuf) {
        *self = SnapshotWindow {
            open: true,
            bundle_dir: Some(bundle_dir),
            machine_name: machine.name.clone(),
            disk: machine.disk.clone(),
            ..Default::default()
        };
        self.refresh();
    }

    pub fn bundle_dir(&self) -> Option<&Path> {
        self.bundle_dir.as_deref()
    }

    /// Headless equivalents of the window's buttons, for `main.rs`'s
    /// `--snapshots` debug verb.
    pub fn snapshots(&self) -> &[Snapshot] {
        &self.list
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    fn refresh(&mut self) {
        match list(&self.disk) {
            Ok(list) => {
                self.list = list;
                self.error = None;
            }
            Err(e) => {
                self.list.clear();
                self.error = Some(e.to_string());
            }
        }
    }

    /// Run one operation and fold its result into the window's state.
    fn run(&mut self, what: &str, result: std::io::Result<()>) {
        match result {
            Ok(()) => {
                self.status = Some(what.to_string());
                self.error = None;
                self.refresh();
            }
            Err(e) => {
                self.status = None;
                self.error = Some(e.to_string());
            }
        }
    }

    pub fn take(&mut self, name: &str) {
        let r = create(&self.disk, name);
        self.run(&format!("took “{name}”"), r);
    }

    pub fn drop_snapshot(&mut self, name: &str) {
        let r = delete(&self.disk, name);
        self.run(&format!("deleted “{name}”"), r);
    }

    pub fn revert(&mut self, name: &str) {
        let r = restore(&self.disk, name);
        self.run(&format!("restored “{name}”"), r);
    }

    /// Renders the window if open. `running` says whether *this*
    /// machine's player is up, in which case every operation is refused:
    /// `qemu-img` writing to an image QEMU has open corrupts it, and
    /// even the listing takes an image lock QEMU already holds. Live
    /// snapshots are step 5c (over the launcher's own QMP socket).
    pub fn show(&mut self, ctx: &egui::Context, running: bool) {
        if !self.open {
            return;
        }
        let mut still_open = true;
        egui::Window::new(format!("Snapshots — {}", self.machine_name))
            .id(egui::Id::new("snapshots"))
            .open(&mut still_open)
            .collapsible(false)
            .default_width(600.0)
            .max_width(760.0)
            .show(ctx, |ui| {
                if running {
                    ui.label("This machine is running. Snapshots of a running machine need the");
                    ui.label("player's monitor, which the launcher doesn't drive yet — shut the");
                    ui.label("guest down first.");
                    return;
                }
                if self.list.is_empty() {
                    ui.label("No snapshots.");
                } else {
                    self.list_ui(ui);
                }
                ui.separator();
                ui.horizontal(|ui| {
                    ui.label("New snapshot");
                    ui.text_edit_singleline(&mut self.new_name);
                    let name = self.new_name.trim().to_string();
                    if ui.add_enabled(!name.is_empty(), egui::Button::new("Take snapshot")).clicked() {
                        self.take(&name);
                        self.new_name.clear();
                    }
                });
                if let Some(status) = &self.status {
                    ui.label(status.clone());
                }
                if let Some(err) = &self.error {
                    ui.colored_label(egui::Color32::RED, err);
                }
            });
        self.open = still_open;
    }

    fn list_ui(&mut self, ui: &mut egui::Ui) {
        let mut restore_name = None;
        let mut delete_name = None;
        // Held in a local so the row loop can borrow `self.list` while
        // still arming the confirmation, and applied after the loop —
        // the list is what a restore/delete replaces.
        let mut confirm = self.confirm_restore.clone();
        let list = &self.list;
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
                        if ui.button("Discard current state?").clicked() {
                            restore_name = Some(snap.name.clone());
                        }
                    } else if ui.button("Restore").clicked() {
                        confirm = Some(snap.name.clone());
                    }
                    if ui.button("Delete").clicked() {
                        delete_name = Some(snap.name.clone());
                    }
                });
                ui.end_row();
            }
        });
        self.confirm_restore = confirm;
        if let Some(name) = restore_name {
            self.confirm_restore = None;
            self.revert(&name);
        }
        if let Some(name) = delete_name {
            self.confirm_restore = None;
            self.drop_snapshot(&name);
        }
    }
}
