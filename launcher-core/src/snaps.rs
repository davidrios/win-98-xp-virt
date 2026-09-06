//! The per-machine snapshot window's model (doc 07 step 5b/5c): which
//! source the list comes from, the in-flight job, and every operation
//! its buttons run.
//!
//! The rules, all of which are the reason this is one implementation:
//!
//! * a running machine is driven through its monitor (`control.rs`),
//!   because `qemu-img` writing to an image QEMU has open corrupts it,
//!   and even listing wants an image lock QEMU already holds;
//! * a stopped one goes through `qemu-img` (`snapshots.rs`);
//! * starting or stopping under the window re-reads the list from the
//!   other source;
//! * live save/load/delete are *jobs*, not synchronous commands — saving
//!   a 512 MB guest's RAM takes a visible moment — so the window polls
//!   rather than the UI thread blocking on QEMU's main loop;
//! * a load runs on a stopped VM and only resumes afterwards if the
//!   guest was actually running: a machine the user had paused shouldn't
//!   come back running because of a restore;
//! * `reload` never clears `error` — a failed operation reports and then
//!   re-reads, and a successful re-read must not wipe that away (it did,
//!   once: a failed live restore looked like it had worked).
//!
//! What each front end still owns is *when* `poll` is called — the egui
//! build does it at the top of every frame because it has a frame
//! anyway, Qt runs a `Timer` that says the interval out loud and stops
//! when there is no job — and how a destructive restore is confirmed.

use crate::bundle::Machine;
use crate::control::{self, Control};
use crate::snapshots::{create, delete, list, restore, Snapshot};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// How often an in-flight job is actually asked about, however often
/// `poll` is called. A frame-driven caller would otherwise ask sixty
/// times a second.
const POLL_INTERVAL: Duration = Duration::from_millis(400);

#[derive(Default)]
pub struct Snapshots {
    /// Whether the window is up.
    pub open: bool,
    /// The bundle directory, so the caller can say whether *this*
    /// machine is the running one.
    bundle_dir: Option<PathBuf>,
    machine_name: String,
    disk: PathBuf,
    list: Vec<Snapshot>,
    error: Option<String>,
    status: Option<String>,
    /// Whether this machine's player is up. Everything branches on it.
    running: bool,
    /// A `snapshot-save`/`-load`/`-delete` job in flight, and whether the
    /// guest has to be resumed once it finishes (a load runs on a
    /// stopped VM).
    job: Option<String>,
    resume_after_job: bool,
    next_job: u64,
    last_poll: Option<Instant>,
}

impl Snapshots {
    pub fn open_for(&mut self, machine: &Machine, bundle_dir: PathBuf, running: bool) {
        *self = Snapshots {
            open: true,
            bundle_dir: Some(bundle_dir),
            machine_name: machine.name.clone(),
            disk: machine.disk.clone(),
            running,
            ..Default::default()
        };
        self.refresh();
    }

    /// The same, from a bundle path — for a front end that addresses its
    /// windows by path rather than by a `Machine` it already holds.
    pub fn open_for_path(&mut self, bundle_path: &Path, running: bool) {
        let dir = bundle_path.parent().unwrap_or(Path::new(".")).to_path_buf();
        match Machine::load(bundle_path) {
            Ok(machine) => self.open_for(&machine, dir, running),
            Err(e) => {
                *self = Snapshots { open: true, bundle_dir: Some(dir), running, ..Default::default() };
                self.error = Some(format!("{}: {e}", bundle_path.display()));
            }
        }
    }

    pub fn title(&self) -> String {
        if self.machine_name.is_empty() {
            "Snapshots".to_string()
        } else {
            format!("Snapshots — {}", self.machine_name)
        }
    }

    pub fn bundle_dir(&self) -> Option<&Path> {
        self.bundle_dir.as_deref()
    }

    pub fn snapshots(&self) -> &[Snapshot] {
        &self.list
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    pub fn status(&self) -> Option<&str> {
        self.status.as_deref()
    }

    pub fn running(&self) -> bool {
        self.running
    }

    /// Whether a live job is in flight: every button is disabled while
    /// one is (a second job on top of it is refused by QEMU anyway) and
    /// the poll timer runs.
    pub fn job_pending(&self) -> bool {
        self.job.is_some()
    }

    /// The machine started or stopped under the window: the same list
    /// now has to come from the other source.
    pub fn set_running(&mut self, running: bool) {
        if running == self.running {
            return;
        }
        self.running = running;
        self.job = None;
        self.refresh();
    }

    /// Connect to the running machine's monitor. A fresh connection per
    /// operation rather than one held across frames: jobs and block
    /// nodes are QEMU-global, not per-monitor, so nothing is lost, and
    /// there is no half-open socket to nurse when a guest shuts down.
    fn control(&self) -> Result<Control, String> {
        let dir = self.bundle_dir.as_deref().ok_or("no machine open")?;
        Control::connect(&control::socket_path(dir))
    }

    /// Re-read the list from whichever source applies. Deliberately does
    /// *not* touch `error` — see this module's header.
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

    /// Run one offline operation and fold its result into the state.
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

    pub fn take(&mut self, name: &str) {
        let name = name.trim();
        if name.is_empty() {
            return;
        }
        if self.running {
            return self.start_job("snapshot-save", name, false);
        }
        let r = create(&self.disk, name).map_err(|e| e.to_string());
        self.run(&format!("took \u{201c}{name}\u{201d}"), r);
    }

    pub fn drop_snapshot(&mut self, name: &str) {
        if self.running {
            return self.start_job("snapshot-delete", name, false);
        }
        let r = delete(&self.disk, name).map_err(|e| e.to_string());
        self.run(&format!("deleted \u{201c}{name}\u{201d}"), r);
    }

    pub fn revert(&mut self, name: &str) {
        if self.running {
            return self.start_job("snapshot-load", name, true);
        }
        let r = restore(&self.disk, name).map_err(|e| e.to_string());
        self.run(&format!("restored \u{201c}{name}\u{201d}"), r);
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
            // requires the VM to be stopped for.
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
                self.status = Some(format!("{command} \u{201c}{tag}\u{201d}\u{2026}"));
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
    pub fn poll_job(&mut self) {
        if self.job.is_none() {
            return;
        }
        if self.last_poll.map(|t| t.elapsed() < POLL_INTERVAL).unwrap_or(false) {
            return;
        }
        self.last_poll = Some(Instant::now());
        self.poll_now();
    }

    /// Poll now, ignoring the throttle — for a caller driving a job from
    /// a loop rather than from frames or a timer (`cli`'s `--snapshots`).
    pub fn poll_job_now(&mut self) {
        self.last_poll = None;
        self.poll_job();
    }

    fn poll_now(&mut self) {
        let Some(job_id) = self.job.clone() else { return };
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

    /// Wait out an in-flight job, for a caller with no frames and no
    /// timer (`cli`'s `--snapshots`, and the egui diagnostic verbs whose
    /// frames run back to back).
    pub fn wait_for_job(&mut self, timeout: Duration) {
        let deadline = Instant::now() + timeout;
        while self.job_pending() && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(200));
            self.poll_job_now();
        }
    }
}
