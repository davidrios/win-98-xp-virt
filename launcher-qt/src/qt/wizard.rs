//! The guided creation wizard (doc 07), as a QObject over
//! `launcher_core::wizard::Form`.
//!
//! The form is the shared one: which defaults follow the family until
//! someone chooses otherwise, the per-family memory clamp, what
//! `build_machine` writes, what `submit` does differently when editing,
//! and the sentences under each row. None of that is here. What is here
//! is the projection onto Q_PROPERTYs, in both directions:
//!
//! * **`publish`** reads the form and writes every property through its
//!   generated setter. Never a direct field assignment — see the header
//!   of `main.rs` for the trap that closes.
//! * **`pull`** copies the plain, two-way-bound text fields back into
//!   the form. A QML `TextField` writes its property and nothing else,
//!   so the form is caught up before anything reads it (`submit`,
//!   `fill_advanced`). The fields with a *consequence* never go this
//!   way: they have no writable property at all, only `choose_*`, which
//!   is what makes the "…_chosen" rule impossible to forget in a new
//!   widget.
//!
//! The combo boxes' labels come from the form's own enums
//! (`family_labels`, `accel_labels`, …) rather than being retyped in
//! QML, and so do the file dialog's name filters — the egui build gets
//! the same strings from the same constants.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
        include!("cxx-qt-lib/qstringlist.h");
        type QStringList = cxx_qt_lib::QStringList;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qproperty(bool, open)]
        #[qproperty(bool, editing)]
        #[qproperty(QString, title)]
        /// An index into `family_labels()`, because that is what a QML
        /// `ComboBox` deals in. Same for `accel`, `cpu_speed` and `boot`.
        #[qproperty(i32, family)]
        #[qproperty(QString, name)]
        #[qproperty(i32, ram_mb)]
        #[qproperty(i32, ram_min)]
        #[qproperty(i32, ram_max)]
        #[qproperty(QString, ram_note)]
        #[qproperty(bool, ram_is_default)]
        #[qproperty(i32, cpu_speed)]
        #[qproperty(QString, cpu_note)]
        #[qproperty(bool, cpu_is_default)]
        #[qproperty(i32, accel)]
        #[qproperty(QString, accel_note)]
        #[qproperty(bool, accel_warning)]
        #[qproperty(bool, accel_is_default)]
        #[qproperty(QString, graphics_note)]
        #[qproperty(bool, graphics_warning)]
        #[qproperty(bool, network)]
        #[qproperty(QString, network_note)]
        #[qproperty(bool, existing_disk)]
        #[qproperty(QString, disk_path)]
        #[qproperty(i32, disk_size_gb)]
        #[qproperty(QString, install_media)]
        #[qproperty(QString, floppy)]
        #[qproperty(i32, boot)]
        #[qproperty(QString, boot_note)]
        /// The chosen shader profile's id, or "" for the app default.
        #[qproperty(QString, shader_profile)]
        #[qproperty(bool, advanced)]
        #[qproperty(QString, advanced_toml)]
        #[qproperty(QString, error)]
        type Wizard = super::WizardRust;

        /// Reset to a fresh "New machine" form and open it.
        #[qinvokable]
        fn open_fresh(self: Pin<&mut Wizard>);

        /// Open the form pre-filled from an existing bundle, to edit it
        /// in place instead of creating a new one.
        #[qinvokable]
        fn open_edit(self: Pin<&mut Wizard>, bundle_path: &QString);

        /// Pick the family, moving whatever nobody has chosen (memory,
        /// the accelerator, the processor, the NIC) to that family's own
        /// default with it.
        #[qinvokable]
        fn choose_family(self: Pin<&mut Wizard>, family: i32);

        /// Set the memory, clamped to the family's range, and remember
        /// that it was chosen so a later family switch can't rewrite it.
        #[qinvokable]
        fn choose_ram(self: Pin<&mut Wizard>, ram_mb: i32);

        /// Put the memory back on the family's default, un-choosing it.
        #[qinvokable]
        fn reset_ram(self: Pin<&mut Wizard>);

        #[qinvokable]
        fn choose_cpu_speed(self: Pin<&mut Wizard>, cpu_speed: i32);

        #[qinvokable]
        fn reset_cpu_speed(self: Pin<&mut Wizard>);

        #[qinvokable]
        fn choose_accel(self: Pin<&mut Wizard>, accel: i32);

        #[qinvokable]
        fn reset_accel(self: Pin<&mut Wizard>);

        #[qinvokable]
        fn choose_network(self: Pin<&mut Wizard>, network: bool);

        /// The boot order. A plain field with no consequence beyond its
        /// own note, but an index like the other combos.
        #[qinvokable]
        fn choose_boot(self: Pin<&mut Wizard>, boot: i32);

        /// A floppy image was typed or browsed to: the boot note depends
        /// on it ("boot from floppy" with no image falls through to the
        /// hard disk), so this is the one text field with a consequence.
        #[qinvokable]
        fn set_floppy_path(self: Pin<&mut Wizard>, floppy: &QString);

        /// Fill the advanced box with the TOML this form currently
        /// describes (or, when editing, the file's exact current text).
        #[qinvokable]
        fn fill_advanced(self: Pin<&mut Wizard>);

        /// Create or save the machine. Returns true on success, having
        /// closed the form; on failure `error` says why and it stays open.
        #[qinvokable]
        fn submit(self: Pin<&mut Wizard>) -> bool;

        /// The `machine.toml` written by the last successful `submit`,
        /// for the caller to rescan around.
        #[qinvokable]
        fn saved_path(self: &Wizard) -> QString;

        /// The combo boxes' labels, from the bundle's own enums rather
        /// than retyped in QML.
        #[qinvokable]
        fn family_labels(self: &Wizard) -> QStringList;

        #[qinvokable]
        fn accel_labels(self: &Wizard) -> QStringList;

        #[qinvokable]
        fn cpu_speed_labels(self: &Wizard) -> QStringList;

        #[qinvokable]
        fn boot_labels(self: &Wizard) -> QStringList;

        /// The file dialog's name filters, from the same constants the
        /// egui build hands `rfd`.
        #[qinvokable]
        fn disk_filter(self: &Wizard) -> QString;

        #[qinvokable]
        fn media_filter(self: &Wizard) -> QString;

        #[qinvokable]
        fn floppy_filter(self: &Wizard) -> QString;
    }
}

use crate::{qs, qs_opt};
use cxx_qt::CxxQtType;
use cxx_qt_lib::{QString, QStringList};
use launcher_core::browse::name_filter;
use launcher_core::bundle::{Accel, Boot, CpuSpeed, Family};
use launcher_core::library;
use launcher_core::wizard::{Form, DISK_FILTER, FLOPPY_FILTER, MEDIA_FILTER};
use std::path::PathBuf;
use std::pin::Pin;

#[derive(Default)]
pub struct WizardRust {
    open: bool,
    editing: bool,
    title: QString,
    family: i32,
    name: QString,
    ram_mb: i32,
    ram_min: i32,
    ram_max: i32,
    ram_note: QString,
    ram_is_default: bool,
    cpu_speed: i32,
    cpu_note: QString,
    cpu_is_default: bool,
    accel: i32,
    accel_note: QString,
    accel_warning: bool,
    accel_is_default: bool,
    graphics_note: QString,
    graphics_warning: bool,
    network: bool,
    network_note: QString,
    existing_disk: bool,
    disk_path: QString,
    disk_size_gb: i32,
    install_media: QString,
    floppy: QString,
    boot: i32,
    boot_note: QString,
    shader_profile: QString,
    advanced: bool,
    advanced_toml: QString,
    error: QString,

    /// The form. Everything above is a projection of it.
    form: Form,
}

/// Index <-> enum, in the order each enum's own `ALL` lists it, which is
/// also the order `*_labels()` hands QML.
fn index_of<T: PartialEq + Copy>(all: &[T], value: T) -> i32 {
    all.iter().position(|v| *v == value).unwrap_or(0) as i32
}

fn at<T: Copy>(all: &[T], index: i32) -> T {
    all[(index.max(0) as usize).min(all.len() - 1)]
}

fn labels(items: impl IntoIterator<Item = &'static str>) -> QStringList {
    let mut list = QStringList::default();
    for item in items {
        list.append(QString::from(item));
    }
    list
}

impl ffi::Wizard {
    fn open_fresh(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().form.open_fresh();
        self.publish();
    }

    fn open_edit(mut self: Pin<&mut Self>, bundle_path: &QString) {
        let path = PathBuf::from(bundle_path.to_string());
        self.as_mut().rust_mut().form.open_edit_path(path);
        self.publish();
    }

    fn choose_family(mut self: Pin<&mut Self>, family: i32) {
        let f = at(&Family::ALL, family);
        self.as_mut().rust_mut().form.choose_family(f);
        self.publish();
    }

    fn choose_ram(mut self: Pin<&mut Self>, ram_mb: i32) {
        self.as_mut().rust_mut().form.choose_ram_mb(ram_mb.max(0) as u32);
        self.publish();
    }

    fn reset_ram(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().form.reset_ram();
        self.publish();
    }

    fn choose_cpu_speed(mut self: Pin<&mut Self>, cpu_speed: i32) {
        let s = at(&CpuSpeed::ALL, cpu_speed);
        self.as_mut().rust_mut().form.choose_cpu_speed(s);
        self.publish();
    }

    fn reset_cpu_speed(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().form.reset_cpu_speed();
        self.publish();
    }

    fn choose_accel(mut self: Pin<&mut Self>, accel: i32) {
        let a = at(&Accel::ALL, accel);
        self.as_mut().rust_mut().form.choose_accel(a);
        self.publish();
    }

    fn reset_accel(mut self: Pin<&mut Self>) {
        self.as_mut().rust_mut().form.reset_accel();
        self.publish();
    }

    fn choose_network(mut self: Pin<&mut Self>, network: bool) {
        self.as_mut().rust_mut().form.choose_network(network);
        self.publish();
    }

    fn choose_boot(mut self: Pin<&mut Self>, boot: i32) {
        let b = at(&Boot::ALL, boot);
        self.as_mut().rust_mut().form.boot = b;
        self.publish();
    }

    fn set_floppy_path(mut self: Pin<&mut Self>, floppy: &QString) {
        self.as_mut().rust_mut().form.floppy = floppy.to_string();
        self.publish();
    }

    fn fill_advanced(mut self: Pin<&mut Self>) {
        self.as_mut().pull();
        self.as_mut().rust_mut().form.fill_advanced();
        self.publish();
    }

    fn submit(mut self: Pin<&mut Self>) -> bool {
        self.as_mut().pull();
        let library_dir = library::default_dir();
        let ok = self.as_mut().rust_mut().form.submit(&library_dir).is_some();
        self.publish();
        ok
    }

    fn saved_path(&self) -> QString {
        self.rust().form.saved_path().map(|p| qs(p.display())).unwrap_or_default()
    }

    fn family_labels(&self) -> QStringList {
        labels(Family::ALL.iter().map(|f| f.label()))
    }

    fn accel_labels(&self) -> QStringList {
        labels(Accel::ALL.iter().map(|a| a.label()))
    }

    fn cpu_speed_labels(&self) -> QStringList {
        labels(CpuSpeed::ALL.iter().map(|s| s.label()))
    }

    fn boot_labels(&self) -> QStringList {
        labels(Boot::ALL.iter().map(|b| b.label()))
    }

    fn disk_filter(&self) -> QString {
        qs(name_filter(DISK_FILTER))
    }

    fn media_filter(&self) -> QString {
        qs(name_filter(MEDIA_FILTER))
    }

    fn floppy_filter(&self) -> QString {
        qs(name_filter(FLOPPY_FILTER))
    }
}

impl ffi::Wizard {
    /// The plain, two-way-bound text fields, back into the form. A QML
    /// `TextField` writes its property and nothing else, so this catches
    /// the form up before anything reads it.
    fn pull(mut self: Pin<&mut Self>) {
        let (name, disk_path, install_media, floppy, advanced_toml, shader_profile) = (
            self.name.to_string(),
            self.disk_path.to_string(),
            self.install_media.to_string(),
            self.floppy.to_string(),
            self.advanced_toml.to_string(),
            self.shader_profile.to_string(),
        );
        let (existing_disk, disk_size_gb, advanced) = (self.existing_disk, self.disk_size_gb, self.advanced);
        let form = &mut self.as_mut().rust_mut().form;
        form.name = name;
        form.disk_path = disk_path;
        form.install_media = install_media;
        form.floppy = floppy;
        form.advanced_toml = advanced_toml;
        form.shader_profile = Some(shader_profile).filter(|p| !p.is_empty());
        form.existing_disk = existing_disk;
        form.disk_size_gb = disk_size_gb.max(1) as u32;
        form.advanced = advanced;
    }

    /// The form, onto the properties — every one through its own setter,
    /// so each notify fires for the values that actually moved. Read
    /// out first, written after: the setters take `&mut self`.
    fn publish(mut self: Pin<&mut Self>) {
        let (
            open,
            editing,
            title,
            family,
            name,
            ram_mb,
            ram_min,
            ram_max,
            ram_note,
            ram_is_default,
            cpu_speed,
            cpu_note,
            cpu_is_default,
        );
        let (accel, accel_note, accel_warning, accel_is_default, network, network_note);
        let (graphics_note, graphics_warning);
        let (existing_disk, disk_path, disk_size_gb, install_media, floppy, boot, boot_note);
        let (shader_profile, advanced, advanced_toml, error);
        {
            let f = &self.rust().form;
            let range = f.ram_range();
            let note = f.accel_note();
            open = f.open;
            editing = f.is_editing();
            title = QString::from(f.title());
            family = index_of(&Family::ALL, f.family());
            name = qs(&f.name);
            ram_mb = f.ram_mb() as i32;
            ram_min = *range.start() as i32;
            ram_max = *range.end() as i32;
            ram_note = qs_opt(f.ram_note());
            ram_is_default = f.ram_is_default();
            cpu_speed = index_of(&CpuSpeed::ALL, f.cpu_speed());
            cpu_note = qs(f.cpu_speed_notes().join("\n"));
            cpu_is_default = f.cpu_speed_is_default();
            accel = index_of(&Accel::ALL, f.accel());
            accel_warning = note.warning;
            accel_note = qs(note.text);
            accel_is_default = f.accel_is_default();
            // Empty on a DOS machine, which has no Direct3D to place.
            let graphics = f.graphics_note();
            graphics_warning = graphics.as_ref().is_some_and(|n| n.warning);
            graphics_note = qs(graphics.map(|n| n.text).unwrap_or_default());
            network = f.network();
            network_note = qs(f.network_notes().join("\n"));
            existing_disk = f.existing_disk;
            disk_path = qs(&f.disk_path);
            disk_size_gb = f.disk_size_gb as i32;
            install_media = qs(&f.install_media);
            floppy = qs(&f.floppy);
            boot = index_of(&Boot::ALL, f.boot);
            boot_note = qs_opt(f.boot_note());
            shader_profile = f.shader_profile.as_deref().map(qs).unwrap_or_default();
            advanced = f.advanced;
            advanced_toml = qs(&f.advanced_toml);
            error = qs_opt(f.error.as_deref());
        }
        self.as_mut().set_open(open);
        self.as_mut().set_editing(editing);
        self.as_mut().set_title(title);
        self.as_mut().set_family(family);
        self.as_mut().set_name(name);
        self.as_mut().set_ram_mb(ram_mb);
        self.as_mut().set_ram_min(ram_min);
        self.as_mut().set_ram_max(ram_max);
        self.as_mut().set_ram_note(ram_note);
        self.as_mut().set_ram_is_default(ram_is_default);
        self.as_mut().set_cpu_speed(cpu_speed);
        self.as_mut().set_cpu_note(cpu_note);
        self.as_mut().set_cpu_is_default(cpu_is_default);
        self.as_mut().set_accel(accel);
        self.as_mut().set_accel_note(accel_note);
        self.as_mut().set_accel_warning(accel_warning);
        self.as_mut().set_accel_is_default(accel_is_default);
        self.as_mut().set_graphics_note(graphics_note);
        self.as_mut().set_graphics_warning(graphics_warning);
        self.as_mut().set_network(network);
        self.as_mut().set_network_note(network_note);
        self.as_mut().set_existing_disk(existing_disk);
        self.as_mut().set_disk_path(disk_path);
        self.as_mut().set_disk_size_gb(disk_size_gb);
        self.as_mut().set_install_media(install_media);
        self.as_mut().set_floppy(floppy);
        self.as_mut().set_boot(boot);
        self.as_mut().set_boot_note(boot_note);
        self.as_mut().set_shader_profile(shader_profile);
        self.as_mut().set_advanced(advanced);
        self.as_mut().set_advanced_toml(advanced_toml);
        self.as_mut().set_error(error);
    }
}
