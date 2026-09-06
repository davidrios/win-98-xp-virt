//! The machine library grid, as a QML model.
//!
//! The egui build draws this in ~90 lines inside `main.rs`'s `ui()`:
//! `egui::Grid`, a row per entry, `if ui.button("Play").clicked()`. Here
//! the same rows are a `QAbstractListModel` and the columns are QML. The
//! logic — `library::scan`, `player::spawn`, the shelf published before a
//! boot, `Child::try_wait` to notice a player exiting — is the same code
//! moved, not rewritten.
//!
//! One structural difference worth recording: egui's immediate mode has
//! no "the list changed" concept, so `main.rs` reassigns `self.entries`
//! and the next frame shows it. Qt needs the model to say so, which is
//! the `beginResetModel` bracket in `refresh` and the `dataChanged` in
//! `poll` — a reset there would drop the view's selection and scroll
//! position every time a player exited.

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

        /// Start a machine's player. Publishes the shared disc shelf for
        /// its drive first, exactly as the egui build's "Play" does.
        #[qinvokable]
        fn play(self: Pin<&mut MachineModel>, row: i32);

        /// Reap any player that exited. QML calls this from a `Timer`
        /// where the egui build did it at the top of every frame — a
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

        /// The shared disc shelf's file, for the shelf window.
        #[qinvokable]
        fn disc_library_path(self: &MachineModel) -> QString;

        /// The shader-profile directory, for the profile manager.
        #[qinvokable]
        fn profile_dir(self: &MachineModel) -> QString;
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

use crate::{bundle, control, disc_library, library, player, qs, shader_library};
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QByteArray, QHash, QHashPair_i32_QByteArray, QModelIndex, QString, QVariant, QVector};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::process::Child;

const ROLE_NAME: i32 = 0;
const ROLE_FAMILY: i32 = 1;
const ROLE_SHADER: i32 = 2;
const ROLE_LOCATION: i32 = 3;
const ROLE_RUNNING: i32 = 4;

pub struct MachineModelRust {
    count: i32,
    library_dir: QString,
    status: QString,
    dir: PathBuf,
    profiles_dir: PathBuf,
    disc_library: PathBuf,
    entries: Vec<library::LibraryEntry>,
    profiles: Vec<shader_library::ProfileEntry>,
    /// Bundle directory -> its player, while running. Absence means "not
    /// running"; `poll` removes an entry the moment the child exits —
    /// the same contract as the egui build's `LauncherApp::running`.
    running: HashMap<PathBuf, Child>,
}

impl Default for MachineModelRust {
    fn default() -> Self {
        let dir = library::default_dir();
        MachineModelRust {
            count: 0,
            library_dir: qs(dir.display()),
            status: QString::default(),
            dir,
            profiles_dir: shader_library::default_dir(),
            disc_library: disc_library::default_path(),
            entries: Vec::new(),
            profiles: Vec::new(),
            running: HashMap::new(),
        }
    }
}

impl MachineModelRust {
    /// The label the "Shader" column shows: the profile's name if the
    /// machine names one that still exists, else a raw `shader`
    /// override's path, else the app default. The egui build's fallback
    /// chain, unchanged.
    fn shader_label(&self, entry: &library::LibraryEntry) -> String {
        entry
            .machine
            .shader_profile
            .as_deref()
            .and_then(|id| self.profiles.iter().find(|e| shader_library::id_of(&e.path) == id))
            .map(|e| e.profile.name.clone())
            .or_else(|| entry.machine.shader.as_ref().map(|p| p.display().to_string()))
            .unwrap_or_else(|| "(default)".to_string())
    }
}

impl ffi::MachineModel {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.entries.len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let Some(entry) = self.entries.get(index.row() as usize) else {
            return QVariant::default();
        };
        match role {
            ROLE_NAME => QVariant::from(&qs(&entry.machine.name)),
            ROLE_FAMILY => QVariant::from(&QString::from(match entry.machine.family {
                bundle::Family::Win98 => "Win98",
                bundle::Family::Xp => "XP",
            })),
            ROLE_SHADER => QVariant::from(&qs(self.rust().shader_label(entry))),
            ROLE_LOCATION => QVariant::from(&qs(entry.dir.display())),
            ROLE_RUNNING => QVariant::from(&self.running.contains_key(&entry.dir)),
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
        {
            let mut this = self.as_mut().rust_mut();
            this.entries = library::scan(&this.dir);
            this.profiles = shader_library::scan(&this.profiles_dir);
        }
        unsafe { self.as_mut().end_reset_model() };

        // Legacy per-machine disc lists onto the shared shelf, as the
        // egui build does at startup. `DiscLibrary::add` deduplicates by
        // path, so running it on every rescan is idempotent.
        let library_path = self.disc_library.clone();
        match disc_library::DiscLibrary::load(&library_path) {
            Ok(mut discs) => {
                if discs.import_legacy(&self.entries) > 0 {
                    if let Err(e) = discs.save(&library_path) {
                        eprintln!("[discs] {}: {e}", library_path.display());
                    }
                }
            }
            Err(e) => eprintln!("[discs] {}: {e}", library_path.display()),
        }

        let count = self.entries.len() as i32;
        self.as_mut().set_count(count);
    }

    fn play(mut self: Pin<&mut Self>, row: i32) {
        let Some(entry) = self.entries.get(row as usize) else { return };
        let dir = entry.dir.clone();
        let machine = entry.machine.clone();
        let library_path = self.disc_library.clone();
        // The monitor socket is derived from the bundle directory, so
        // every window that wants live control finds it again without
        // the app carrying it around.
        let socket = control::socket_path(&dir);
        let shelf = control::shelf_path(&dir);
        publish_shelf(&library_path, &shelf);
        match player::spawn(&machine, Some(&socket), Some(&shelf)) {
            Ok(child) => {
                self.as_mut().rust_mut().running.insert(dir, child);
                self.as_mut().set_status(qs(format!("started {}", machine.name)));
                touch_row(self, row);
            }
            Err(e) => self.as_mut().set_status(qs(format!("{}: {e}", dir.display()))),
        }
    }

    fn poll(mut self: Pin<&mut Self>) {
        let mut ended: Vec<PathBuf> = Vec::new();
        {
            let mut this = self.as_mut().rust_mut();
            this.running.retain(|dir, child| match child.try_wait() {
                Ok(None) => true,
                Ok(Some(status)) => {
                    eprintln!("[launcher-qt] {} exited: {status}", dir.display());
                    ended.push(dir.clone());
                    false
                }
                Err(e) => {
                    eprintln!("[launcher-qt] {}: {e}", dir.display());
                    ended.push(dir.clone());
                    false
                }
            });
        }
        for dir in ended {
            if let Some(row) = self.entries.iter().position(|e| e.dir == dir) {
                touch_row(self.as_mut(), row as i32);
            }
        }
    }

    fn bundle_path(&self, row: i32) -> QString {
        match self.entries.get(row as usize) {
            Some(entry) => qs(entry.dir.join(library::BUNDLE_FILE).display()),
            None => QString::default(),
        }
    }

    fn is_running(&self, row: i32) -> bool {
        self.entries.get(row as usize).map(|e| self.running.contains_key(&e.dir)).unwrap_or(false)
    }

    fn disc_library_path(&self) -> QString {
        qs(self.disc_library.display())
    }

    fn profile_dir(&self) -> QString {
        qs(self.rust().profiles_dir.display())
    }
}

/// Tell attached views that one row's data moved — the `running` flag is
/// the only thing that changes without a rescan.
fn touch_row(mut model: Pin<&mut ffi::MachineModel>, row: i32) {
    let index = unsafe { model.as_ref().model_index(row, 0, &QModelIndex::default()) };
    let roles = QVector::<i32>::default();
    unsafe { model.as_mut().data_changed(&index, &index, &roles) };
}

/// Write the shared shelf out in the flat form a machine's ATAPI drive
/// reads (`cdshelf/cdshelf_proto.h`), so the in-guest CDSHELF program
/// sees the same discs the launcher does. Straight from the egui build's
/// `main.rs`; failing to publish is not fatal — the machine still runs,
/// its drive just reports an empty shelf.
pub fn publish_shelf(library_path: &Path, shelf_path: &Path) {
    match disc_library::DiscLibrary::load(library_path) {
        Ok(library) => {
            if let Err(e) = disc_library::write_shelf_file(&library, shelf_path) {
                eprintln!("[discs] {}: {e}", shelf_path.display());
            }
        }
        Err(e) => eprintln!("[discs] {}: {e}", library_path.display()),
    }
}
