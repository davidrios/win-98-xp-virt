//! The disc shelf (doc 07), as a QML model.
//!
//! One object, two modes, exactly as `launcher/src/discshelf.rs`: opened
//! on its own it manages the shared shelf (add, label, remove); opened
//! from a machine's row it also carries that machine's two disc
//! decisions — which disc is in the drive at **boot** (a bundle edit)
//! and, while it runs, which to **insert** now (a monitor command).
//!
//! The egui version keeps a `dirty` flag and writes the shelf at the end
//! of any frame that changed something, because an editable label in
//! immediate mode would otherwise write the file on every keystroke's
//! borrow of the list. Qt's `TextField` has an `editingFinished` signal,
//! so the label commit is a single `set_label` call and the flag is
//! gone: the file is written when a label is actually finished, not when
//! a frame happens to end.

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
        /// Whether a machine's context is loaded (the Boot column and the
        /// live row only exist then).
        #[qproperty(bool, for_machine)]
        #[qproperty(bool, running)]
        #[qproperty(QString, boot_label)]
        #[qproperty(bool, has_boot)]
        #[qproperty(QString, status)]
        #[qproperty(QString, error)]
        /// The newest guest-tools ISO this checkout built, or "" — doc
        /// 07's one-click attach, with nothing to browse for.
        #[qproperty(QString, guest_tools_iso)]
        type DiscModel = super::DiscModelRust;

        #[qinvokable]
        #[cxx_override]
        fn row_count(self: &DiscModel, parent: &QModelIndex) -> i32;

        #[qinvokable]
        #[cxx_override]
        fn data(self: &DiscModel, index: &QModelIndex, role: i32) -> QVariant;

        #[qinvokable]
        #[cxx_override]
        fn role_names(self: &DiscModel) -> QHash_i32_QByteArray;

        /// Open on the shared shelf alone, with no machine context.
        #[qinvokable]
        fn open_library(self: Pin<&mut DiscModel>, library_path: &QString);

        /// Open for one machine: the same shelf, plus its boot-disc
        /// choice and (while running) live insert.
        #[qinvokable]
        fn open_for(self: Pin<&mut DiscModel>, bundle_path: &QString, library_path: &QString, running: bool);

        #[qinvokable]
        fn add(self: Pin<&mut DiscModel>, path: &QString);

        #[qinvokable]
        fn add_guest_tools(self: Pin<&mut DiscModel>);

        #[qinvokable]
        fn remove(self: Pin<&mut DiscModel>, row: i32);

        #[qinvokable]
        fn set_label(self: Pin<&mut DiscModel>, row: i32, label: &QString);

        #[qinvokable]
        fn set_boot(self: Pin<&mut DiscModel>, row: i32);

        #[qinvokable]
        fn clear_boot(self: Pin<&mut DiscModel>);

        #[qinvokable]
        fn insert_live(self: Pin<&mut DiscModel>, row: i32);

        #[qinvokable]
        fn eject_live(self: Pin<&mut DiscModel>);

        /// Whether the shelf was written since this was last asked — the
        /// cue to republish it to every running machine's drive so the
        /// in-guest CDSHELF program sees a disc the moment it's added.
        #[qinvokable]
        fn take_saved(self: Pin<&mut DiscModel>) -> bool;

        /// Republish the shelf for one running machine's bundle
        /// directory. `Main.qml` calls this for each running row after
        /// `take_saved`, which is how the egui build's `main.rs` does it.
        #[qinvokable]
        fn publish_to(self: &DiscModel, bundle_dir: &QString);

        /// The bundle the window has open, so the caller can tell whether
        /// *that* machine is the running one.
        #[qinvokable]
        fn bundle_dir(self: &DiscModel) -> QString;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        /// # Safety
        /// Inherited; paired with `end_reset_model`.
        #[inherit]
        unsafe fn begin_reset_model(self: Pin<&mut DiscModel>);

        /// # Safety
        /// Inherited; pairs with `begin_reset_model`.
        #[inherit]
        unsafe fn end_reset_model(self: Pin<&mut DiscModel>);
    }
}

use crate::bundle::Machine;
use crate::disc_library::{self, DiscLibrary};
use crate::{control, qs};
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QByteArray, QHash, QHashPair_i32_QByteArray, QModelIndex, QString, QVariant};
use std::path::{Path, PathBuf};
use std::pin::Pin;

const ROLE_LABEL: i32 = 0;
const ROLE_NAME: i32 = 1;
const ROLE_DIR: i32 = 2;
const ROLE_PATH: i32 = 3;
const ROLE_IS_BOOT: i32 = 4;

/// `Clone` so a change can be made on a copy and written back through
/// the setters — see `wizard.rs`'s header for why a direct write to a
/// Q_PROPERTY field is silent.
#[derive(Default, Clone)]
pub struct DiscModelRust {
    count: i32,
    open: bool,
    title: QString,
    for_machine: bool,
    running: bool,
    boot_label: QString,
    has_boot: bool,
    status: QString,
    error: QString,
    guest_tools_iso: QString,

    library_path: PathBuf,
    library: DiscLibrary,
    saved: bool,
    /// `None` when opened for the shelf itself rather than a machine.
    bundle_path: Option<PathBuf>,
    /// The machine's current boot disc, mirrored so a click updates the
    /// row markers before anything is rescanned.
    boot: Option<PathBuf>,
}

impl DiscModelRust {
    fn load(&mut self, library_path: &Path) {
        self.library_path = library_path.to_path_buf();
        match DiscLibrary::load(library_path) {
            Ok(library) => self.library = library,
            // A corrupt shelf is reported, never silently replaced with
            // an empty one — the next save would then destroy it.
            Err(e) => self.error = qs(format!("{}: {e}", library_path.display())),
        }
    }

    fn save(&mut self) {
        match self.library.save(&self.library_path) {
            Ok(()) => self.saved = true,
            Err(e) => self.error = qs(e),
        }
    }

    fn bundle_dir(&self) -> Option<&Path> {
        self.bundle_path.as_ref().and_then(|p| p.parent())
    }

    /// Everything derived from the state above, before it is applied:
    /// the row count, the boot-disc label, and whether this checkout has
    /// a guest-tools ISO to offer.
    fn recompute(&mut self) {
        self.count = self.library.discs.len() as i32;
        self.has_boot = self.boot.is_some();
        self.boot_label = qs(self
            .boot
            .as_deref()
            .map(disc_library::default_label)
            .unwrap_or_else(|| "(empty tray)".to_string()));
        self.guest_tools_iso =
            disc_library::guest_tools_iso().map(|p| qs(p.display())).unwrap_or_default();
    }
}

impl ffi::DiscModel {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.library.discs.len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let Some(disc) = self.library.discs.get(index.row() as usize) else {
            return QVariant::default();
        };
        match role {
            ROLE_LABEL => QVariant::from(&qs(&disc.label)),
            // The file name and its directory as separate roles, because
            // a full path in one column makes every row read
            // `/home/…/…/…` once it is elided. Same reasoning as the egui
            // build's two-column split; QML elides the directory and
            // shows the whole path as a tooltip.
            ROLE_NAME => QVariant::from(&qs(disc
                .path
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_else(|| disc.path.display().to_string()))),
            ROLE_DIR => QVariant::from(&qs(disc
                .path
                .parent()
                .map(|p| p.display().to_string())
                .unwrap_or_default())),
            ROLE_PATH => QVariant::from(&qs(disc.path.display())),
            ROLE_IS_BOOT => QVariant::from(&(self.boot.as_deref() == Some(disc.path.as_path()))),
            _ => QVariant::default(),
        }
    }

    fn role_names(&self) -> QHash<QHashPair_i32_QByteArray> {
        let mut roles = QHash::<QHashPair_i32_QByteArray>::default();
        roles.insert(ROLE_LABEL, QByteArray::from("label"));
        roles.insert(ROLE_NAME, QByteArray::from("name"));
        roles.insert(ROLE_DIR, QByteArray::from("dir"));
        roles.insert(ROLE_PATH, QByteArray::from("path"));
        roles.insert(ROLE_IS_BOOT, QByteArray::from("isBoot"));
        roles
    }

    fn open_library(self: Pin<&mut Self>, library_path: &QString) {
        let path = PathBuf::from(library_path.to_string());
        let mut state = DiscModelRust { open: true, title: QString::from("Disc shelf"), ..Default::default() };
        state.load(&path);
        state.recompute();
        self.apply(state, true);
    }

    fn open_for(self: Pin<&mut Self>, bundle_path: &QString, library_path: &QString, running: bool) {
        let bundle = PathBuf::from(bundle_path.to_string());
        let library = PathBuf::from(library_path.to_string());
        let machine = Machine::load(&bundle);
        let mut state = DiscModelRust { open: true, for_machine: true, running, ..Default::default() };
        state.load(&library);
        match &machine {
            Ok(m) => {
                state.boot = m.boot_disc().cloned();
                state.title = qs(format!("Discs — {}", m.name));
            }
            Err(e) => {
                state.error = qs(format!("{}: {e}", bundle.display()));
                state.title = QString::from("Discs");
            }
        }
        state.bundle_path = Some(bundle);
        state.recompute();
        self.apply(state, true);
    }

    fn add(self: Pin<&mut Self>, path: &QString) {
        let path = PathBuf::from(path.to_string().trim());
        if path.as_os_str().is_empty() {
            return;
        }
        let label = disc_library::default_label(&path);
        let mut state = self.rust().clone();
        if state.library.add(path) {
            state.save();
            state.status = qs(format!("added {label}"));
            state.error = QString::default();
        } else {
            // The same image added twice is one entry, not two rows that
            // then disagree about their labels.
            state.status = qs(format!("{label} is already on the shelf"));
        }
        state.recompute();
        self.apply(state, true);
    }

    fn add_guest_tools(mut self: Pin<&mut Self>) {
        match disc_library::guest_tools_iso() {
            Some(iso) => {
                let q = qs(iso.display());
                self.add(&q);
            }
            None => self
                .as_mut()
                .set_error(QString::from("no guest-tools ISO built (guest-tools/build-wrappers.sh)")),
        }
    }

    fn remove(self: Pin<&mut Self>, row: i32) {
        let mut state = self.rust().clone();
        if row < 0 || row as usize >= state.library.discs.len() {
            return;
        }
        let disc = state.library.discs.remove(row as usize);
        state.save();
        state.status = qs(format!("removed {}", disc.label));
        state.error = QString::default();
        state.recompute();
        self.apply(state, true);
    }

    fn set_label(self: Pin<&mut Self>, row: i32, label: &QString) {
        let label = label.to_string();
        let mut state = self.rust().clone();
        let Some(disc) = state.library.discs.get_mut(row as usize) else { return };
        if disc.label == label {
            return;
        }
        disc.label = label;
        state.save();
        state.recompute();
        self.apply(state, true);
    }

    /// Set the open machine's boot disc, writing the bundle.
    fn set_boot(self: Pin<&mut Self>, row: i32) {
        let path = self.library.discs.get(row as usize).map(|d| d.path.clone());
        let Some(path) = path else { return };
        self.write_boot(Some(path));
    }

    fn clear_boot(self: Pin<&mut Self>) {
        self.write_boot(None);
    }

    fn insert_live(self: Pin<&mut Self>, row: i32) {
        let Some(disc) = self.library.discs.get(row as usize).map(|d| d.path.clone()) else { return };
        let name = disc.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        self.live(&format!("inserted {name}"), move |c| c.insert_disc(&disc));
    }

    fn eject_live(self: Pin<&mut Self>) {
        self.live("ejected", |c| c.eject_disc());
    }

    fn take_saved(mut self: Pin<&mut Self>) -> bool {
        std::mem::take(&mut self.as_mut().rust_mut().saved)
    }

    fn publish_to(&self, bundle_dir: &QString) {
        let dir = PathBuf::from(bundle_dir.to_string());
        crate::qt::machines::publish_shelf(&self.library_path, &control::shelf_path(&dir));
    }

    fn bundle_dir(&self) -> QString {
        self.rust().bundle_dir().map(|d| qs(d.display())).unwrap_or_default()
    }
}

impl ffi::DiscModel {
    /// Re-read the bundle rather than editing a `Machine` captured when
    /// the window opened: the wizard may have saved the same bundle in
    /// between, and only the boot disc is this window's to change.
    fn write_boot(mut self: Pin<&mut Self>, path: Option<PathBuf>) {
        let Some(bundle_path) = self.bundle_path.clone() else {
            self.as_mut().set_error(QString::from("no machine open"));
            return;
        };
        let result = Machine::load(&bundle_path).and_then(|mut m| {
            m.disc = path.clone();
            m.discs.clear();
            m.save(&bundle_path)
        });
        let mut state = self.rust().clone();
        match result {
            Ok(()) => {
                state.boot = path.clone();
                state.status = qs(match &path {
                    Some(p) => format!("boots with {}", disc_library::default_label(p)),
                    None => "boots with an empty tray".to_string(),
                });
                state.error = QString::default();
            }
            Err(e) => state.error = qs(e),
        }
        state.recompute();
        self.apply(state, true);
    }

    /// Run one operation on the running machine's monitor. A fresh
    /// connection each time: jobs and block nodes are QEMU-global, so
    /// nothing is lost, and there's no half-open socket to nurse when a
    /// guest shuts down between the frame that drew the button and the
    /// click on it.
    fn live(self: Pin<&mut Self>, done: &str, op: impl FnOnce(&mut control::Control) -> Result<(), String>) {
        let Some(dir) = self.rust().bundle_dir().map(|d| d.to_path_buf()) else {
            return self.apply_error("no machine open");
        };
        let result = control::Control::connect(&control::socket_path(&dir)).and_then(|mut c| op(&mut c));
        let mut state = self.rust().clone();
        match result {
            Ok(()) => {
                state.status = qs(done);
                state.error = QString::default();
            }
            Err(e) => {
                state.status = QString::default();
                state.error = qs(e);
            }
        }
        self.apply(state, false);
    }

    fn apply_error(self: Pin<&mut Self>, message: &str) {
        let mut state = self.rust().clone();
        state.status = QString::default();
        state.error = qs(message);
        self.apply(state, false);
    }

    /// Write a whole new state in. `reset_rows` brackets the row change
    /// in `beginResetModel`/`endResetModel`; every Q_PROPERTY goes
    /// through its own setter, because a direct write to one of those
    /// fields changes what QML reads without telling it (see the header
    /// of `wizard.rs`).
    fn apply(mut self: Pin<&mut Self>, state: DiscModelRust, reset_rows: bool) {
        if reset_rows {
            // Safety: paired with `end_reset_model` below.
            unsafe { self.as_mut().begin_reset_model() };
        }
        {
            let mut this = self.as_mut().rust_mut();
            this.library_path = state.library_path.clone();
            this.library = state.library.clone();
            this.saved = state.saved;
            this.bundle_path = state.bundle_path.clone();
            this.boot = state.boot.clone();
        }
        if reset_rows {
            unsafe { self.as_mut().end_reset_model() };
        }
        self.as_mut().set_count(state.count);
        self.as_mut().set_open(state.open);
        self.as_mut().set_title(state.title);
        self.as_mut().set_for_machine(state.for_machine);
        self.as_mut().set_running(state.running);
        self.as_mut().set_boot_label(state.boot_label);
        self.as_mut().set_has_boot(state.has_boot);
        self.as_mut().set_status(state.status);
        self.as_mut().set_error(state.error);
        self.as_mut().set_guest_tools_iso(state.guest_tools_iso);
    }
}
