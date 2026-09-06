//! The per-machine snapshot window (doc 07 step 5b/5c): the state
//! machine — which source the list comes from, the in-flight job, the
//! pending restore confirmation — and the egui that draws it.
//!
//! The `Snapshot` type and the `qemu-img` calls under it are
//! `snapshots.rs`, which this is the only toolkit-bound half of.

use crate::snapshots::{create, delete, list, restore, Snapshot};
use std::path::Path;
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
    /// Whether this machine's player is up, as of the last `show`.
    /// Everything below branches on it: a running machine is driven
    /// through its monitor (`control.rs`), a stopped one through
    /// `qemu-img`, and switching between the two re-reads the list.
    running: bool,
    /// A `snapshot-save`/`-load`/`-delete` job in flight, and whether the
    /// guest has to be resumed once it finishes (a load runs on a
    /// stopped VM). These are jobs, not synchronous commands — saving a
    /// 512 MB guest's RAM takes a visible moment — so the window polls
    /// instead of the UI thread blocking on QEMU's main loop.
    job: Option<String>,
    resume_after_job: bool,
    next_job: u64,
    last_poll: Option<std::time::Instant>,
}

impl SnapshotWindow {
    pub fn open_for(&mut self, machine: &crate::bundle::Machine, bundle_dir: std::path::PathBuf, running: bool) {
        *self = SnapshotWindow {
            open: true,
            bundle_dir: Some(bundle_dir),
            machine_name: machine.name.clone(),
            disk: machine.disk.clone(),
            running,
            ..Default::default()
        };
        self.refresh();
    }

    /// Connect to the running machine's monitor. A fresh connection per
    /// operation rather than one held across frames: jobs and block
    /// nodes are QEMU-global, not per-monitor, so nothing is lost, and
    /// there's no half-open socket to nurse when a guest shuts down.
    fn control(&self) -> Result<crate::control::Control, String> {
        let dir = self.bundle_dir.as_deref().ok_or("no machine open")?;
        crate::control::Control::connect(&crate::control::socket_path(dir))
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

    pub fn status(&self) -> Option<&str> {
        self.status.as_deref()
    }

    /// Whether a live snapshot job is still in flight (`--snapshots
    /// --live` waits on this instead of a window's repaint tick).
    pub fn job_pending(&self) -> bool {
        self.job.is_some()
    }

    /// Poll an in-flight job now, ignoring the repaint-rate throttle —
    /// for a caller driving the job from a loop rather than frames.
    pub fn poll_job_now(&mut self) {
        self.last_poll = None;
        self.poll_job();
    }

    /// Re-read the list from whichever source applies. Deliberately does
    /// *not* touch `error`: an operation that failed reports its own
    /// error and then re-reads, and a successful re-read must not wipe
    /// that away (it did, once — a failed live restore looked like it
    /// had worked).
    fn reload(&mut self) -> Result<(), String> {
        let result = if self.running {
            self.control().and_then(|mut c| c.disk_node(&self.disk)).map(|(_, snapshots)| snapshots)
        } else {
            list(&self.disk).map_err(|e| e.to_string())
        };
        match result {
            Ok(list) => {
                self.list = list;
                Ok(())
            }
            Err(e) => {
                self.list.clear();
                Err(e)
            }
        }
    }

    /// Re-read and make the result the window's current message — for
    /// opening the window, or when the machine started or stopped.
    fn refresh(&mut self) {
        self.error = self.reload().err();
    }

    /// Run one operation and fold its result into the window's state.
    fn run(&mut self, what: &str, result: Result<(), String>) {
        match result {
            Ok(()) => {
                self.status = Some(what.to_string());
                self.error = self.reload().err();
            }
            Err(e) => {
                self.status = None;
                self.error = Some(e);
            }
        }
    }

    /// Start a live snapshot job (`command` is the QMP verb) and leave
    /// the window polling it.
    fn start_job(&mut self, command: &str, tag: &str, stop_first: bool) {
        self.next_job += 1;
        let job_id = format!("launcher-{}", self.next_job);
        let disk = self.disk.clone();
        let mut resume_after = false;
        let result = self.control().and_then(|mut c| {
            let (node, _) = c.disk_node(&disk)?;
            // A load replaces the guest's CPU/RAM state, which QEMU
            // requires the VM to be stopped for. Only resume afterwards
            // if it was actually running: a machine the user had paused
            // shouldn't come back running because of a restore.
            if stop_first {
                resume_after = c.is_running()?;
                c.set_running(false)?;
            }
            c.start_snapshot_job(command, &job_id, tag, &node)
        });
        match result {
            Ok(()) => {
                self.job = Some(job_id);
                self.resume_after_job = resume_after;
                self.error = None;
                self.status = Some(format!("{command} “{tag}”…"));
            }
            Err(e) => {
                self.resume_after_job = false;
                self.status = None;
                self.error = Some(e);
            }
        }
    }

    /// Check an in-flight job, at most a couple of times a second. A
    /// concluded job stays around until dismissed, which is where its
    /// error (if any) comes from.
    fn poll_job(&mut self) {
        let Some(job_id) = self.job.clone() else {
            return;
        };
        if self.last_poll.map(|t| t.elapsed() < std::time::Duration::from_millis(400)).unwrap_or(false) {
            return;
        }
        self.last_poll = Some(std::time::Instant::now());
        let state = self.control().and_then(|mut c| c.job(&job_id));
        let (status, error) = match state {
            Ok(Some(state)) => state,
            // Gone already (or the monitor went away with the guest):
            // stop polling rather than spinning on a job that can't
            // report anything.
            Ok(None) => {
                self.job = None;
                self.refresh();
                return;
            }
            Err(e) => {
                self.job = None;
                self.error = Some(e);
                return;
            }
        };
        if status != "concluded" {
            return;
        }
        self.job = None;
        let resume = std::mem::take(&mut self.resume_after_job);
        let result = self.control().and_then(|mut c| {
            c.dismiss_job(&job_id)?;
            if resume {
                c.set_running(true)?;
            }
            Ok(())
        });
        // The job's own error wins over anything the tidy-up hit.
        let outcome = error.map(Err).unwrap_or(result);
        let reload = self.reload();
        match outcome.and(reload) {
            Ok(()) => {
                self.status = Some("done".into());
                self.error = None;
            }
            Err(e) => {
                self.status = None;
                self.error = Some(e);
            }
        }
    }

    pub fn take(&mut self, name: &str) {
        if self.running {
            return self.start_job("snapshot-save", name, false);
        }
        let r = create(&self.disk, name).map_err(|e| e.to_string());
        self.run(&format!("took “{name}”"), r);
    }

    pub fn drop_snapshot(&mut self, name: &str) {
        if self.running {
            return self.start_job("snapshot-delete", name, false);
        }
        let r = delete(&self.disk, name).map_err(|e| e.to_string());
        self.run(&format!("deleted “{name}”"), r);
    }

    pub fn revert(&mut self, name: &str) {
        if self.running {
            return self.start_job("snapshot-load", name, true);
        }
        let r = restore(&self.disk, name).map_err(|e| e.to_string());
        self.run(&format!("restored “{name}”"), r);
    }

    /// Renders the window if open. `running` says whether *this*
    /// machine's player is up: if it is, every operation goes through
    /// the machine's monitor (`control.rs`), because `qemu-img` writing
    /// to an image QEMU has open corrupts it and even the listing wants
    /// an image lock QEMU already holds.
    pub fn show(&mut self, ctx: &egui::Context, running: bool) {
        if !self.open {
            return;
        }
        if running != self.running {
            // Started or stopped under us: the same list now has to come
            // from the other source.
            self.running = running;
            self.job = None;
            self.refresh();
        }
        self.poll_job();
        let mut still_open = true;
        egui::Window::new(format!("Snapshots — {}", self.machine_name))
            .id(egui::Id::new("snapshots"))
            .open(&mut still_open)
            .collapsible(false)
            .default_width(600.0)
            .max_width(760.0)
            .show(ctx, |ui| {
                if self.running {
                    ui.label("Live: this machine is running, so a snapshot also stores its RAM and CPU state.");
                    ui.separator();
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
                    // A job in flight owns the guest's state; a second
                    // one on top of it is refused by QEMU anyway.
                    let ready = !name.is_empty() && self.job.is_none();
                    if ui.add_enabled(ready, egui::Button::new("Take snapshot")).clicked() {
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
        let idle = self.job.is_none();
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
            self.revert(&name);
        }
        if let Some(name) = delete_name {
            self.confirm_restore = None;
            self.drop_snapshot(&name);
        }
    }
}
