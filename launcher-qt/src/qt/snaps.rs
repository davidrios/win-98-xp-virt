//! Snapshots (doc 07), as a QML model over
//! `launcher_core::snaps::Snapshots`.
//!
//! Every rule that matters is in the model and shared with the egui
//! build: a running machine is driven through its monitor because
//! `qemu-img` writing to an image QEMU has open corrupts it, a stopped
//! one through `qemu-img`, starting or stopping re-reads the list from
//! the other source, live save/load/delete are *jobs* that have to be
//! polled, a load runs on a stopped VM and only resumes if the guest was
//! actually running, and a re-read never clears an error.
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

use crate::{qs, qs_opt};
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QByteArray, QHash, QHashPair_i32_QByteArray, QModelIndex, QString, QVariant};
use launcher_core::snaps::Snapshots;
use std::path::PathBuf;
use std::pin::Pin;

const ROLE_NAME: i32 = 0;
const ROLE_DATE: i32 = 1;
const ROLE_SIZE: i32 = 2;

#[derive(Default)]
pub struct SnapshotModelRust {
    count: i32,
    open: bool,
    title: QString,
    running: bool,
    busy: bool,
    status: QString,
    error: QString,

    /// The window's state machine. Everything above is a projection.
    model: Snapshots,
}

impl ffi::SnapshotModel {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.rust().model.snapshots().len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let Some(snap) = self.rust().model.snapshots().get(index.row() as usize) else {
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

    fn open_for(mut self: Pin<&mut Self>, bundle_path: &QString, running: bool) {
        let path = PathBuf::from(bundle_path.to_string());
        self.as_mut().with_rows(|m| m.open_for_path(&path, running));
        self.publish();
    }

    fn set_live(mut self: Pin<&mut Self>, running: bool) {
        self.as_mut().with_rows(|m| m.set_running(running));
        self.publish();
    }

    fn take(mut self: Pin<&mut Self>, name: &QString) {
        let name = name.to_string();
        self.as_mut().with_rows(|m| m.take(&name));
        self.publish();
    }

    fn revert(mut self: Pin<&mut Self>, name: &QString) {
        let name = name.to_string();
        self.as_mut().with_rows(|m| m.revert(&name));
        self.publish();
    }

    fn drop_snapshot(mut self: Pin<&mut Self>, name: &QString) {
        let name = name.to_string();
        self.as_mut().with_rows(|m| m.drop_snapshot(&name));
        self.publish();
    }

    fn poll(mut self: Pin<&mut Self>) {
        if !self.rust().model.job_pending() {
            return;
        }
        self.as_mut().with_rows(|m| m.poll_job());
        self.publish();
    }
}

impl ffi::SnapshotModel {
    /// Run an operation that may change the rows, bracketed so attached
    /// views are told.
    fn with_rows(mut self: Pin<&mut Self>, op: impl FnOnce(&mut Snapshots)) {
        // Safety: paired with `end_reset_model` immediately below.
        unsafe { self.as_mut().begin_reset_model() };
        op(&mut self.as_mut().rust_mut().model);
        unsafe { self.as_mut().end_reset_model() };
    }

    /// The model, onto the properties — every one through its own
    /// setter (see the header of `main.rs`).
    fn publish(mut self: Pin<&mut Self>) {
        let (count, open, title, running, busy, status, error);
        {
            let m = &self.rust().model;
            count = m.snapshots().len() as i32;
            open = m.open;
            title = qs(m.title());
            running = m.running();
            busy = m.job_pending();
            status = qs_opt(m.status());
            error = qs_opt(m.error());
        }
        self.as_mut().set_count(count);
        self.as_mut().set_open(open);
        self.as_mut().set_title(title);
        self.as_mut().set_running(running);
        self.as_mut().set_busy(busy);
        self.as_mut().set_status(status);
        self.as_mut().set_error(error);
    }
}
