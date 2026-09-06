//! The disc shelf (doc 07), as a QML model over
//! `launcher_core::shelf::Shelf`.
//!
//! One object, two modes, both the shelf's: opened on its own it manages
//! the shared collection (add, label, remove); opened from a machine's
//! row it also carries that machine's two disc decisions — which disc is
//! in the drive at **boot** (a bundle edit) and, while it runs, which to
//! **insert** now (a monitor command). None of that is here.
//!
//! What is here: the rows as a `QAbstractListModel`, and the one place
//! Qt genuinely does better than immediate mode — a `TextField` has an
//! `editingFinished`, so a label commit is one `set_label` plus one
//! `flush`, where the egui build has to set a dirty flag while drawing
//! and write at the end of the frame or it would save the file on every
//! keystroke.

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
        /// The file dialog's name filter for a disc image, from the
        /// shelf's own constant.
        #[qproperty(QString, disc_filter)]
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
        /// `take_saved`, which is what the egui build does too.
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

use crate::{qs, qs_opt};
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QByteArray, QHash, QHashPair_i32_QByteArray, QModelIndex, QString, QVariant};
use launcher_core::browse::name_filter;
use launcher_core::disc_library::{self, DISC_FILTER};
use launcher_core::shelf::Shelf;
use std::path::PathBuf;
use std::pin::Pin;

const ROLE_LABEL: i32 = 0;
const ROLE_NAME: i32 = 1;
const ROLE_DIR: i32 = 2;
const ROLE_PATH: i32 = 3;
const ROLE_IS_BOOT: i32 = 4;

#[derive(Default)]
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
    disc_filter: QString,

    /// The shelf. Everything above is a projection of it.
    shelf: Shelf,
}

impl ffi::DiscModel {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.rust().shelf.discs().len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let shelf = &self.rust().shelf;
        let Some(disc) = shelf.discs().get(index.row() as usize) else {
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
            ROLE_IS_BOOT => QVariant::from(&(shelf.boot() == Some(disc.path.as_path()))),
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

    fn open_library(mut self: Pin<&mut Self>, library_path: &QString) {
        let path = PathBuf::from(library_path.to_string());
        self.as_mut().with_rows(|shelf| shelf.open_library(&path));
        self.publish();
    }

    fn open_for(mut self: Pin<&mut Self>, bundle_path: &QString, library_path: &QString, running: bool) {
        let bundle = PathBuf::from(bundle_path.to_string());
        let library = PathBuf::from(library_path.to_string());
        self.as_mut().with_rows(|shelf| shelf.open_for_path(bundle, &library));
        self.as_mut().set_running(running);
        self.publish();
    }

    fn add(mut self: Pin<&mut Self>, path: &QString) {
        let path = PathBuf::from(path.to_string().trim());
        if path.as_os_str().is_empty() {
            return;
        }
        self.as_mut().with_rows(|shelf| {
            shelf.add(path);
            shelf.flush_reporting();
        });
        self.publish();
    }

    fn add_guest_tools(mut self: Pin<&mut Self>) {
        self.as_mut().with_rows(|shelf| {
            shelf.add_guest_tools();
            shelf.flush_reporting();
        });
        self.publish();
    }

    fn remove(mut self: Pin<&mut Self>, row: i32) {
        if row < 0 {
            return;
        }
        self.as_mut().with_rows(|shelf| {
            shelf.remove_row(row as usize);
            shelf.flush_reporting();
        });
        self.publish();
    }

    fn set_label(mut self: Pin<&mut Self>, row: i32, label: &QString) {
        if row < 0 {
            return;
        }
        let label = label.to_string();
        self.as_mut().with_rows(|shelf| {
            shelf.set_label(row as usize, &label);
            shelf.flush_reporting();
        });
        self.publish();
    }

    fn set_boot(mut self: Pin<&mut Self>, row: i32) {
        if row < 0 {
            return;
        }
        self.as_mut().with_rows(|shelf| {
            shelf.set_boot_row(row as usize);
        });
        self.publish();
    }

    fn clear_boot(mut self: Pin<&mut Self>) {
        self.as_mut().with_rows(|shelf| {
            shelf.set_boot(None);
        });
        self.publish();
    }

    fn insert_live(mut self: Pin<&mut Self>, row: i32) {
        if row < 0 {
            return;
        }
        self.as_mut().rust_mut().shelf.insert_live_row(row as usize);
        self.publish();
    }

    fn eject_live(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().shelf.eject_live();
        self.publish();
    }

    fn take_saved(mut self: Pin<&mut Self>) -> bool {
        self.as_mut().rust_mut().shelf.take_saved()
    }

    fn publish_to(&self, bundle_dir: &QString) {
        self.rust().shelf.publish_to(&PathBuf::from(bundle_dir.to_string()));
    }

    fn bundle_dir(&self) -> QString {
        self.rust().shelf.bundle_dir().map(|d| qs(d.display())).unwrap_or_default()
    }
}

impl ffi::DiscModel {
    /// Run a shelf operation that may change the rows, bracketed in
    /// `beginResetModel`/`endResetModel` so attached views are told.
    fn with_rows(mut self: Pin<&mut Self>, op: impl FnOnce(&mut Shelf)) {
        // Safety: paired with `end_reset_model` immediately below.
        unsafe { self.as_mut().begin_reset_model() };
        op(&mut self.as_mut().rust_mut().shelf);
        unsafe { self.as_mut().end_reset_model() };
    }

    /// The shelf, onto the properties — every one through its own
    /// setter, because a direct write to one of those fields changes
    /// what QML reads without telling it (see the header of `main.rs`).
    fn publish(mut self: Pin<&mut Self>) {
        let (count, open, title, for_machine, boot_label, has_boot, status, error, iso, filter);
        {
            let shelf = &self.rust().shelf;
            count = shelf.discs().len() as i32;
            open = shelf.open;
            title = qs(shelf.title());
            for_machine = shelf.for_machine();
            boot_label = qs(shelf.boot_label());
            has_boot = shelf.boot().is_some();
            status = qs_opt(shelf.status());
            error = qs_opt(shelf.error());
            iso = disc_library::guest_tools_iso().map(|p| qs(p.display())).unwrap_or_default();
            filter = qs(name_filter(DISC_FILTER));
        }
        self.as_mut().set_count(count);
        self.as_mut().set_open(open);
        self.as_mut().set_title(title);
        self.as_mut().set_for_machine(for_machine);
        self.as_mut().set_boot_label(boot_label);
        self.as_mut().set_has_boot(has_boot);
        self.as_mut().set_status(status);
        self.as_mut().set_error(error);
        self.as_mut().set_guest_tools_iso(iso);
        self.as_mut().set_disc_filter(filter);
    }
}
