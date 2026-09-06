//! The machine library grid, as a QML model over
//! `launcher_core::machines::Machines` — the scan, the running-player
//! map, what "Play" does (publish the shelf, derive the monitor socket
//! from the bundle directory, spawn) and the reap that notices a player
//! exiting.
//!
//! One structural difference worth recording: egui's immediate mode has
//! no "the list changed" concept, so its grid just redraws from the
//! model's entries. Qt needs the model to say so, which is the
//! `beginResetModel` bracket in `refresh` and the `dataChanged` in
//! `poll` — a reset there would drop the view's selection and scroll
//! position every time a player exited. That is exactly why `reap`
//! returns the rows that moved.

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
        include!("cxx-qt-lib/qvector.h");
        type QVector_i32 = cxx_qt_lib::QVector<i32>;
    }

    unsafe extern "C++" {
        include!(<QtCore/QAbstractListModel>);
        type QAbstractListModel;
    }

    // `#[auto_cxx_name]` gives every `snake_case` item below its
    // `camelCase` name in QML, which is what a QML reader expects to
    // type; the Rust side stays idiomatic Rust.
    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[base = QAbstractListModel]
        #[qml_element]
        #[qproperty(i32, count)]
        #[qproperty(QString, library_dir)]
        #[qproperty(QString, status)]
        type MachineModel = super::MachineModelRust;

        #[qinvokable]
        #[cxx_override]
        fn row_count(self: &MachineModel, parent: &QModelIndex) -> i32;

        #[qinvokable]
        #[cxx_override]
        fn data(self: &MachineModel, index: &QModelIndex, role: i32) -> QVariant;

        #[qinvokable]
        #[cxx_override]
        fn role_names(self: &MachineModel) -> QHash_i32_QByteArray;

        /// Rescan the library directory from disk.
        #[qinvokable]
        fn refresh(self: Pin<&mut MachineModel>);

        /// Rescan only the shader-profile library, after the profile
        /// manager saved or deleted one: that changes the "Shader"
        /// column but not the rows.
        #[qinvokable]
        fn refresh_profiles(self: Pin<&mut MachineModel>);

        /// Start a machine's player.
        #[qinvokable]
        fn play(self: Pin<&mut MachineModel>, row: i32);

        /// Reap any player that exited. QML calls this from a `Timer`
        /// where the egui build does it at the top of every frame — a
        /// child process still has no way to push the news.
        #[qinvokable]
        fn poll(self: Pin<&mut MachineModel>);

        /// The `machine.toml` of a row: how every other window is
        /// addressed, since they re-read the bundle rather than being
        /// handed a copy of it.
        #[qinvokable]
        fn bundle_path(self: &MachineModel, row: i32) -> QString;

        #[qinvokable]
        fn is_running(self: &MachineModel, row: i32) -> bool;

        /// Whether the machine in a given bundle directory is up — how a
        /// per-machine window, which knows its bundle and not its row,
        /// asks.
        #[qinvokable]
        fn is_running_dir(self: &MachineModel, dir: &QString) -> bool;

        /// The shared disc shelf's file, for the shelf window.
        #[qinvokable]
        fn disc_library_path(self: &MachineModel) -> QString;

        /// The shader-profile directory, for the profile manager.
        #[qinvokable]
        fn profile_dir(self: &MachineModel) -> QString;

        /// Republish the shared shelf to every running machine's drive,
        /// after the shelf window reports it wrote.
        #[qinvokable]
        fn republish_shelf(self: &MachineModel);
    }

    #[auto_cxx_name]
    extern "RustQt" {
        /// # Safety
        /// Inherited from QAbstractItemModel; must be paired with `end_reset_model`.
        #[inherit]
        unsafe fn begin_reset_model(self: Pin<&mut MachineModel>);

        /// # Safety
        /// Inherited from QAbstractItemModel; pairs with `begin_reset_model`.
        #[inherit]
        unsafe fn end_reset_model(self: Pin<&mut MachineModel>);

        /// # Safety
        /// Inherited from QAbstractItemModel.
        #[inherit]
        #[cxx_name = "index"]
        unsafe fn model_index(self: &MachineModel, row: i32, column: i32, parent: &QModelIndex) -> QModelIndex;

        /// # Safety
        /// Inherited from QAbstractItemModel: tells attached views that
        /// the given rows' data moved.
        #[inherit]
        unsafe fn data_changed(
            self: Pin<&mut MachineModel>,
            top_left: &QModelIndex,
            bottom_right: &QModelIndex,
            roles: &QVector_i32,
        );
    }
}

use crate::qs;
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QByteArray, QHash, QHashPair_i32_QByteArray, QModelIndex, QString, QVariant, QVector};
use launcher_core::machines::Machines;
use std::path::PathBuf;
use std::pin::Pin;

const ROLE_NAME: i32 = 0;
const ROLE_FAMILY: i32 = 1;
const ROLE_SHADER: i32 = 2;
const ROLE_LOCATION: i32 = 3;
const ROLE_RUNNING: i32 = 4;

pub struct MachineModelRust {
    count: i32,
    library_dir: QString,
    status: QString,

    /// The library. Everything above is a projection of it.
    machines: Machines,
}

impl Default for MachineModelRust {
    fn default() -> Self {
        let machines = Machines::default();
        MachineModelRust {
            count: 0,
            library_dir: qs(machines.library_dir.display()),
            status: QString::default(),
            machines,
        }
    }
}

impl ffi::MachineModel {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.rust().machines.len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let machines = &self.rust().machines;
        let row = index.row() as usize;
        let Some(entry) = machines.entries().get(row) else {
            return QVariant::default();
        };
        match role {
            ROLE_NAME => QVariant::from(&qs(&entry.machine.name)),
            ROLE_FAMILY => QVariant::from(&QString::from(entry.machine.family.label())),
            ROLE_SHADER => QVariant::from(&qs(machines.shader_label(entry))),
            ROLE_LOCATION => QVariant::from(&qs(entry.dir.display())),
            ROLE_RUNNING => QVariant::from(&machines.is_running(row)),
            _ => QVariant::default(),
        }
    }

    fn role_names(&self) -> QHash<QHashPair_i32_QByteArray> {
        let mut roles = QHash::<QHashPair_i32_QByteArray>::default();
        roles.insert(ROLE_NAME, QByteArray::from("name"));
        roles.insert(ROLE_FAMILY, QByteArray::from("family"));
        roles.insert(ROLE_SHADER, QByteArray::from("shader"));
        roles.insert(ROLE_LOCATION, QByteArray::from("location"));
        roles.insert(ROLE_RUNNING, QByteArray::from("running"));
        roles
    }

    fn refresh(mut self: Pin<&mut Self>) {
        // Safety: paired with `end_reset_model` immediately below.
        unsafe { self.as_mut().begin_reset_model() };
        self.as_mut().rust_mut().machines.refresh();
        unsafe { self.as_mut().end_reset_model() };
        let (count, dir) = {
            let m = &self.rust().machines;
            (m.len() as i32, qs(m.library_dir.display()))
        };
        self.as_mut().set_count(count);
        self.as_mut().set_library_dir(dir);
    }

    fn refresh_profiles(mut self: Pin<&mut Self>) {
        // The rows are the same machines; only the "Shader" column moved.
        self.as_mut().rust_mut().machines.refresh_profiles();
        let count = self.rust().machines.len() as i32;
        for row in 0..count {
            touch_row(self.as_mut(), row);
        }
    }

    fn play(mut self: Pin<&mut Self>, row: i32) {
        if row < 0 {
            return;
        }
        let result = self.as_mut().rust_mut().machines.play(row as usize);
        match result {
            Ok(status) => {
                self.as_mut().set_status(qs(status));
                touch_row(self, row);
            }
            Err(e) => self.as_mut().set_status(qs(e)),
        }
    }

    fn poll(mut self: Pin<&mut Self>) {
        let ended = self.as_mut().rust_mut().machines.reap();
        for row in ended {
            touch_row(self.as_mut(), row as i32);
        }
    }

    fn bundle_path(&self, row: i32) -> QString {
        if row < 0 {
            return QString::default();
        }
        self.rust().machines.bundle_path(row as usize).map(|p| qs(p.display())).unwrap_or_default()
    }

    fn is_running(&self, row: i32) -> bool {
        row >= 0 && self.rust().machines.is_running(row as usize)
    }

    fn is_running_dir(&self, dir: &QString) -> bool {
        self.rust().machines.is_running_dir(&PathBuf::from(dir.to_string()))
    }

    fn disc_library_path(&self) -> QString {
        qs(self.rust().machines.disc_library_path.display())
    }

    fn profile_dir(&self) -> QString {
        qs(self.rust().machines.profiles_dir.display())
    }

    fn republish_shelf(&self) {
        self.rust().machines.republish_shelf();
    }
}

/// Tell attached views that one row's data moved — the `running` flag
/// and the shader label are what change without a rescan.
fn touch_row(mut model: Pin<&mut ffi::MachineModel>, row: i32) {
    let index = unsafe { model.as_ref().model_index(row, 0, &QModelIndex::default()) };
    let roles = QVector::<i32>::default();
    unsafe { model.as_mut().data_changed(&index, &index, &roles) };
}
