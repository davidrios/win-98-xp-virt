//! Snapshots (doc 07), as a QML model.
//!
//! The state machine is `launcher/src/snapshots.rs`'s `SnapshotWindow`,
//! re-expressed here because that struct interleaves its state with the
//! egui that draws it (see `src/snapshots.rs` in this crate for why the
//! rest of that file *is* shared). The rules are unchanged and they are
//! the part that matters:
//!
//! * a running machine is driven through its monitor (`control.rs`),
//!   because `qemu-img` writing to an image QEMU has open corrupts it,
//!   and even listing wants a lock QEMU already holds;
//! * a stopped one goes through `qemu-img`;
//! * starting or stopping under the window re-reads the list from the
//!   other source;
//! * live save/load/delete are *jobs*, not synchronous commands (saving
//!   a 512 MB guest's RAM takes a visible moment), so the window polls;
//! * a load runs on a stopped VM and only resumes afterwards if it was
//!   actually running;
//! * `reload` never clears `error` — a failed operation reports and then
//!   re-reads, and a successful re-read must not wipe that away.
//!
//! The poll timer is the one thing Qt does better here: egui polls at
//! most twice a second from inside `show`, which only runs because the
//! window is repainting anyway. A QML `Timer` says the interval out loud
//! and stops when there is no job.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
        include!("cxx-qt-lib/qvariant.h");
        type QVariant = cxx_qt_lib::QVariant;
        include!("cxx-qt-lib/qmodelindex.h");
        type QModelIndex = cxx_qt_lib::QModelIndex;
        include!("cxx-qt-lib/qhash.h");
        type QHash_i32_QByteArray = cxx_qt_lib::QHash<cxx_qt_lib::QHashPair_i32_QByteArray>;
    }

    unsafe extern "C++" {
        include!(<QtCore/QAbstractListModel>);
        type QAbstractListModel;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[base = QAbstractListModel]
        #[qml_element]
        #[qproperty(i32, count)]
        #[qproperty(bool, open)]
        #[qproperty(QString, title)]
        /// Whether this machine's player is up. Everything branches on it.
        #[qproperty(bool, running)]
        /// A live job is in flight: every button is disabled and the poll
        /// timer runs.
        #[qproperty(bool, busy)]
        #[qproperty(QString, status)]
        #[qproperty(QString, error)]
        type SnapshotModel = super::SnapshotModelRust;

        #[qinvokable]
        #[cxx_override]
        fn row_count(self: &SnapshotModel, parent: &QModelIndex) -> i32;

        #[qinvokable]
        #[cxx_override]
        fn data(self: &SnapshotModel, index: &QModelIndex, role: i32) -> QVariant;

        #[qinvokable]
        #[cxx_override]
        fn role_names(self: &SnapshotModel) -> QHash_i32_QByteArray;

        #[qinvokable]
        fn open_for(self: Pin<&mut SnapshotModel>, bundle_path: &QString, running: bool);

        /// The machine started or stopped under the window: the same list
        /// now has to come from the other source.
        #[qinvokable]
        fn set_live(self: Pin<&mut SnapshotModel>, running: bool);

        #[qinvokable]
        fn take(self: Pin<&mut SnapshotModel>, name: &QString);

        #[qinvokable]
        fn revert(self: Pin<&mut SnapshotModel>, name: &QString);

        #[qinvokable]
        fn drop_snapshot(self: Pin<&mut SnapshotModel>, name: &QString);

        /// Check an in-flight job. Driven by a QML `Timer` while `busy`.
        #[qinvokable]
        fn poll(self: Pin<&mut SnapshotModel>);
    }

    #[auto_cxx_name]
    extern "RustQt" {
        /// # Safety
        /// Inherited; paired with `end_reset_model`.
        #[inherit]
        unsafe fn begin_reset_model(self: Pin<&mut SnapshotModel>);

        /// # Safety
        /// Inherited; pairs with `begin_reset_model`.
        #[inherit]
        unsafe fn end_reset_model(self: Pin<&mut SnapshotModel>);
    }
}

use crate::bundle::Machine;
use crate::snapshots::{self, Snapshot};
use crate::{control, qs};
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QByteArray, QHash, QHashPair_i32_QByteArray, QModelIndex, QString, QVariant};
use std::path::PathBuf;
use std::pin::Pin;

const ROLE_NAME: i32 = 0;
const ROLE_DATE: i32 = 1;
const ROLE_SIZE: i32 = 2;

/// `Clone` so a change can be made on a copy and written back through
/// the setters — see `wizard.rs`'s header for why a direct write to a
/// Q_PROPERTY field is silent.
#[derive(Default, Clone)]
pub struct SnapshotModelRust {
    count: i32,
    open: bool,
    title: QString,
    running: bool,
    busy: bool,
    status: QString,
    error: QString,

    bundle_dir: Option<PathBuf>,
    disk: PathBuf,
    list: Vec<Snapshot>,
    /// A `snapshot-save`/`-load`/`-delete` job in flight, and whether the
    /// guest has to be resumed once it finishes.
    job: Option<String>,
    resume_after_job: bool,
    next_job: u64,
}

impl SnapshotModelRust {
    /// Connect to the running machine's monitor. A fresh connection per
    /// operation: jobs and block nodes are QEMU-global, so nothing is
    /// lost, and there's no half-open socket to nurse.
    fn control(&self) -> Result<control::Control, String> {
        let dir = self.bundle_dir.as_deref().ok_or("no machine open")?;
        control::Control::connect(&control::socket_path(dir))
    }

    /// Re-read the list from whichever source applies. Deliberately does
    /// *not* touch `error` — see this module's header.
    fn reload(&mut self) -> Result<(), String> {
        let result = if self.running {
            self.control().and_then(|mut c| c.disk_node(&self.disk)).map(|(_, snapshots)| snapshots)
        } else {
            snapshots::list(&self.disk).map_err(|e| e.to_string())
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

    /// The two values derived from the state above, before it is
    /// applied: the row count, and whether a live job is in flight (which
    /// disables every button and runs the poll timer).
    fn recompute(&mut self) {
        self.count = self.list.len() as i32;
        self.busy = self.job.is_some();
    }

    /// Run one offline operation and fold its result into the state.
    fn run(&mut self, what: &str, result: Result<(), String>) {
        match result {
            Ok(()) => {
                self.status = qs(what);
                self.error = self.reload().err().map(qs).unwrap_or_default();
            }
            Err(e) => {
                self.status = QString::default();
                self.error = qs(e);
            }
        }
    }

    /// Start a live snapshot job (`command` is the QMP verb) and leave
    /// the model polling it.
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
                self.error = QString::default();
                self.status = qs(format!("{command} \u{201c}{tag}\u{201d}\u{2026}"));
            }
            Err(e) => {
                self.resume_after_job = false;
                self.status = QString::default();
                self.error = qs(e);
            }
        }
    }

    fn poll_job(&mut self) {
        let Some(job_id) = self.job.clone() else { return };
        let state = self.control().and_then(|mut c| c.job(&job_id));
        let (status, error) = match state {
            Ok(Some(state)) => state,
            // Gone already (or the monitor went with the guest): stop
            // polling rather than spinning on a job that can't report.
            Ok(None) => {
                self.job = None;
                self.error = self.reload().err().map(qs).unwrap_or_default();
                return;
            }
            Err(e) => {
                self.job = None;
                self.error = qs(e);
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
                self.status = QString::from("done");
                self.error = QString::default();
            }
            Err(e) => {
                self.status = QString::default();
                self.error = qs(e);
            }
        }
    }
}

impl ffi::SnapshotModel {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.list.len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let Some(snap) = self.list.get(index.row() as usize) else {
            return QVariant::default();
        };
        match role {
            ROLE_NAME => QVariant::from(&qs(&snap.name)),
            ROLE_DATE => QVariant::from(&qs(snap.date_label())),
            ROLE_SIZE => QVariant::from(&qs(snap.size_label())),
            _ => QVariant::default(),
        }
    }

    fn role_names(&self) -> QHash<QHashPair_i32_QByteArray> {
        let mut roles = QHash::<QHashPair_i32_QByteArray>::default();
        roles.insert(ROLE_NAME, QByteArray::from("name"));
        roles.insert(ROLE_DATE, QByteArray::from("taken"));
        roles.insert(ROLE_SIZE, QByteArray::from("vmState"));
        roles
    }


    fn open_for(self: Pin<&mut Self>, bundle_path: &QString, running: bool) {
        let path = PathBuf::from(bundle_path.to_string());
        let mut state = SnapshotModelRust { open: true, running, ..Default::default() };
        state.bundle_dir = path.parent().map(|p| p.to_path_buf());
        match Machine::load(&path) {
            Ok(m) => {
                state.disk = m.disk.clone();
                state.title = qs(format!("Snapshots — {}", m.name));
                state.error = state.reload().err().map(qs).unwrap_or_default();
            }
            Err(e) => {
                state.title = QString::from("Snapshots");
                state.error = qs(format!("{}: {e}", path.display()));
            }
        }
        state.recompute();
        self.apply(state);
    }

    fn set_live(self: Pin<&mut Self>, running: bool) {
        if self.running == running {
            return;
        }
        // Started or stopped under the window: the same list now has to
        // come from the other source.
        let mut state = self.rust().clone();
        state.running = running;
        state.job = None;
        state.error = state.reload().err().map(qs).unwrap_or_default();
        state.recompute();
        self.apply(state);
    }

    fn take(self: Pin<&mut Self>, name: &QString) {
        let name = name.to_string();
        let name = name.trim().to_string();
        if name.is_empty() {
            return;
        }
        let mut state = self.rust().clone();
        if state.running {
            state.start_job("snapshot-save", &name, false);
        } else {
            let r = snapshots::create(&state.disk, &name).map_err(|e| e.to_string());
            state.run(&format!("took \u{201c}{name}\u{201d}"), r);
        }
        state.recompute();
        self.apply(state);
    }

    fn revert(self: Pin<&mut Self>, name: &QString) {
        let name = name.to_string();
        let mut state = self.rust().clone();
        if state.running {
            state.start_job("snapshot-load", &name, true);
        } else {
            let r = snapshots::restore(&state.disk, &name).map_err(|e| e.to_string());
            state.run(&format!("restored \u{201c}{name}\u{201d}"), r);
        }
        state.recompute();
        self.apply(state);
    }

    fn drop_snapshot(self: Pin<&mut Self>, name: &QString) {
        let name = name.to_string();
        let mut state = self.rust().clone();
        if state.running {
            state.start_job("snapshot-delete", &name, false);
        } else {
            let r = snapshots::delete(&state.disk, &name).map_err(|e| e.to_string());
            state.run(&format!("deleted \u{201c}{name}\u{201d}"), r);
        }
        state.recompute();
        self.apply(state);
    }

    fn poll(self: Pin<&mut Self>) {
        if self.job.is_none() {
            return;
        }
        let mut state = self.rust().clone();
        state.poll_job();
        state.recompute();
        self.apply(state);
    }
}

impl ffi::SnapshotModel {
    /// Write a whole new state in: the rows under a reset bracket, then
    /// every Q_PROPERTY through its own setter — a direct write to one
    /// of those fields would change what QML reads without telling it
    /// (see the header of `wizard.rs`).
    fn apply(mut self: Pin<&mut Self>, state: SnapshotModelRust) {
        // Safety: paired with `end_reset_model` immediately below.
        unsafe { self.as_mut().begin_reset_model() };
        {
            let mut this = self.as_mut().rust_mut();
            this.bundle_dir = state.bundle_dir.clone();
            this.disk = state.disk.clone();
            this.list = state.list.clone();
            this.job = state.job.clone();
            this.resume_after_job = state.resume_after_job;
            this.next_job = state.next_job;
        }
        unsafe { self.as_mut().end_reset_model() };
        self.as_mut().set_count(state.count);
        self.as_mut().set_open(state.open);
        self.as_mut().set_title(state.title);
        self.as_mut().set_running(state.running);
        self.as_mut().set_busy(state.busy);
        self.as_mut().set_status(state.status);
        self.as_mut().set_error(state.error);
    }
}
