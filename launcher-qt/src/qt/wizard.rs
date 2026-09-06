//! The guided creation wizard (doc 07), as a QObject.
//!
//! `launcher/src/wizard.rs` is 453 lines, of which roughly 150 are
//! `egui::ComboBox`/`DragValue`/`checkbox` calls and the rest is the
//! form's *behaviour*: which defaults follow the family until someone
//! chooses otherwise (`ram_chosen`, `accel_chosen`), the per-family RAM
//! clamp, what `build_machine` writes, what `submit` does differently
//! when editing. All of that behaviour is here, near enough line for
//! line; the widgets are `qml/WizardDialog.qml`.
//!
//! The one shape change Qt forces: egui reads a widget's new value and
//! then compares it to the old one in the same frame (`if self.family !=
//! was { … }`), which is how "changing the family moves the memory
//! default with it" is written there. A QML property setter has no such
//! before/after pair, so every field that has a *consequence* gets an
//! explicit invokable (`choose_family`, `choose_ram`, `choose_accel`,
//! `choose_network`) and the plain fields stay two-way-bound properties.
//! This is more code in the bridge and much less in the view, and it
//! makes the "…_chosen" logic impossible to forget in a new widget.
//!
//! **The trap this file exists to encode:** cxx-qt stores a Q_PROPERTY
//! *in* the Rust struct, and the generated setter is
//!
//! ```ignore
//! if self.field == value { return; }   // no binding loops
//! self.as_mut().rust_mut().field = value;
//! self.field_changed();
//! ```
//!
//! So writing `rust_mut().open = true` and then "publishing" it with
//! `set_open(true)` emits **nothing** — the setter sees the value it is
//! being asked for is already there. Every window in this port opened
//! once and then stopped reacting because of exactly that. The rule
//! here, and in `discs.rs` / `snaps.rs` / `shaders.rs`: a whole-form
//! change is built as a *value* and handed to `apply`, which writes
//! every property through its setter while the struct still holds the
//! old one.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qproperty(bool, open)]
        #[qproperty(bool, editing)]
        #[qproperty(QString, title)]
        /// 0 = Win98, 1 = XP — an index, because that is what a QML
        /// `ComboBox` deals in.
        #[qproperty(i32, family)]
        #[qproperty(QString, name)]
        #[qproperty(i32, ram_mb)]
        #[qproperty(i32, ram_min)]
        #[qproperty(i32, ram_max)]
        #[qproperty(QString, ram_note)]
        #[qproperty(bool, ram_is_default)]
        /// 0 = Automatic, 1 = KVM (required), 2 = Emulation.
        #[qproperty(i32, accel)]
        #[qproperty(QString, accel_note)]
        #[qproperty(bool, accel_warning)]
        #[qproperty(bool, accel_is_default)]
        #[qproperty(bool, network)]
        #[qproperty(QString, network_note)]
        #[qproperty(bool, existing_disk)]
        #[qproperty(QString, disk_path)]
        #[qproperty(i32, disk_size_gb)]
        #[qproperty(QString, install_media)]
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
        /// acceleration) to that family's own default with it.
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
        fn choose_accel(self: Pin<&mut Wizard>, accel: i32);

        #[qinvokable]
        fn reset_accel(self: Pin<&mut Wizard>);

        #[qinvokable]
        fn choose_network(self: Pin<&mut Wizard>, network: bool);

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
    }
}

use crate::bundle::{self, Accel, Family, Machine};
use crate::{library, player, qs};
use cxx_qt::CxxQtType;
use cxx_qt_lib::QString;
use std::path::PathBuf;
use std::pin::Pin;

/// `Clone` so a mutation can be made on a copy and applied through the
/// setters — see this module's header.
#[derive(Clone)]
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
    accel: i32,
    accel_note: QString,
    accel_warning: bool,
    accel_is_default: bool,
    network: bool,
    network_note: QString,
    existing_disk: bool,
    disk_path: QString,
    disk_size_gb: i32,
    install_media: QString,
    shader_profile: QString,
    advanced: bool,
    advanced_toml: QString,
    error: QString,

    /// Whether the memory field holds a value someone chose — until it
    /// does, switching family moves it to that family's default.
    ram_chosen: bool,
    /// The same, for the accelerator.
    accel_chosen: bool,
    library_dir: PathBuf,
    /// When editing: the bundle being written back, the raw `shader`
    /// override this form doesn't expose (so a quick edit can't silently
    /// discard it), and the file's exact current text for the advanced
    /// box. `launcher/src/wizard.rs`'s `EditTarget`.
    edit_path: Option<PathBuf>,
    edit_shader: Option<PathBuf>,
    edit_toml: String,
    saved_path: PathBuf,
}

impl Default for WizardRust {
    fn default() -> Self {
        let mut w = WizardRust {
            open: false,
            editing: false,
            title: QString::from("New machine"),
            family: 0,
            name: QString::default(),
            ram_mb: bundle::default_ram_mb(Family::Win98) as i32,
            ram_min: 0,
            ram_max: 0,
            ram_note: QString::default(),
            ram_is_default: true,
            accel: accel_index(bundle::default_accel(Family::Win98)),
            accel_note: QString::default(),
            accel_warning: false,
            accel_is_default: true,
            network: true,
            network_note: QString::default(),
            existing_disk: false,
            disk_path: QString::default(),
            disk_size_gb: 2,
            install_media: QString::default(),
            shader_profile: QString::default(),
            advanced: false,
            advanced_toml: QString::default(),
            error: QString::default(),
            ram_chosen: false,
            accel_chosen: false,
            library_dir: library::default_dir(),
            edit_path: None,
            edit_shader: None,
            edit_toml: String::new(),
            saved_path: PathBuf::new(),
        };
        w.recompute();
        w
    }
}

fn family_of(index: i32) -> Family {
    if index == 1 {
        Family::Xp
    } else {
        Family::Win98
    }
}

fn accel_of(index: i32) -> Accel {
    match index {
        1 => Accel::Kvm,
        2 => Accel::Tcg,
        _ => Accel::Auto,
    }
}

fn accel_index(accel: Accel) -> i32 {
    match accel {
        Accel::Auto => 0,
        Accel::Kvm => 1,
        Accel::Tcg => 2,
    }
}

impl WizardRust {
    /// Everything derived from the current field values: the range the
    /// memory field is clamped to, and the three explanatory lines the
    /// egui build computes inline while drawing. Called after every
    /// change that could move one of them, so QML's bindings see a
    /// notify signal instead of having to poll.
    fn recompute(&mut self) {
        let family = family_of(self.family);
        let range = bundle::ram_mb_range(family);
        let (min, max) = (*range.start() as i32, *range.end() as i32);
        self.ram_min = min;
        self.ram_max = max;
        self.ram_mb = self.ram_mb.clamp(min, max);
        self.ram_is_default = self.ram_mb == bundle::default_ram_mb(family) as i32;
        self.ram_note = QString::from(if family == Family::Win98 && self.ram_mb >= max {
            "512 MB is Win98's ceiling (doc 06): more and it does not boot."
        } else {
            ""
        });

        let have_kvm = player::kvm_available();
        let accel = accel_of(self.accel);
        self.accel_is_default = accel == bundle::default_accel(family);
        self.accel_warning = matches!((accel, have_kvm), (Accel::Kvm, false));
        let mut note = match (accel, have_kvm) {
            (Accel::Auto, true) => "KVM is available on this host and will be used.".to_string(),
            (Accel::Auto, false) => "No KVM on this host: this machine will be emulated.".to_string(),
            (Accel::Kvm, true) => "KVM is available on this host.".to_string(),
            (Accel::Kvm, false) => "No KVM on this host: this machine will refuse to start.".to_string(),
            (Accel::Tcg, _) => "Emulated: the era-CPU behaviour everything here is tuned for.".to_string(),
        };
        if family == Family::Win98 && accel != Accel::Tcg && have_kvm {
            note.push_str("\nWin98 runs at host speed under KVM, which its own fast-CPU bugs dislike.");
        }
        self.accel_note = qs(note);

        self.network_note = QString::from(if self.network {
            "Outbound only, through the host (user-mode NAT): nothing on the network can reach the guest.\nThese are unpatched systems — don't browse the web on one."
        } else {
            "No network adapter at all: Windows won't see a card or ask for its driver."
        });
    }

    /// The `Machine` the current fields describe, given the disk to use
    /// (a fresh disk's path isn't known until it's created). Shared by
    /// the advanced box's default and `submit`, so the two cannot
    /// disagree — `launcher/src/wizard.rs`'s `build_machine`.
    fn build_machine(&self, disk: PathBuf) -> Machine {
        let family = family_of(self.family);
        let name = self.name.to_string();
        let mut machine = match &self.edit_path {
            Some(_) => Machine {
                name,
                family,
                ram_mb: self.ram_mb as u32,
                accel: Some(accel_of(self.accel)),
                network: self.network,
                disk,
                disc: None,
                discs: Vec::new(),
                shader_profile: None,
                shader: self.edit_shader.clone(),
            },
            None => Machine::reference(family, name, disk),
        };
        // `ram_chosen` decides, not the field's contents: a form that
        // never went through the family picker never had the chance to
        // move the default along with it.
        machine.ram_mb =
            if self.ram_chosen { self.ram_mb as u32 } else { bundle::default_ram_mb(family) };
        machine.accel =
            Some(if self.accel_chosen { accel_of(self.accel) } else { bundle::default_accel(family) });
        machine.network = self.network;
        let profile = self.shader_profile.to_string();
        machine.shader_profile = Some(profile).filter(|p| !p.is_empty());
        // The single slot this form has is the machine's *boot* disc;
        // everything else lives on the shared shelf.
        machine.disc = Some(self.install_media.to_string())
            .map(|m| m.trim().to_string())
            .filter(|m| !m.is_empty())
            .map(PathBuf::from);
        machine
    }

    fn preview_toml(&self) -> String {
        if self.edit_path.is_some() {
            return self.edit_toml.clone();
        }
        let disk: PathBuf =
            if self.existing_disk { self.disk_path.to_string().into() } else { "disk.qcow2".into() };
        toml::to_string_pretty(&self.build_machine(disk)).unwrap_or_default()
    }

    /// `launcher/src/wizard.rs`'s `submit`, unchanged in substance.
    fn write(&self) -> std::io::Result<PathBuf> {
        let name = self.name.to_string();
        if name.trim().is_empty() {
            return Err(std::io::Error::other("a name is required"));
        }
        let advanced_toml = self.advanced_toml.to_string();
        if let Some(bundle_path) = &self.edit_path {
            if self.advanced {
                // Validate before writing: a bad hand-edit shouldn't
                // silently corrupt the library with an unreadable bundle.
                toml::from_str::<Machine>(&advanced_toml).map_err(std::io::Error::other)?;
                std::fs::write(bundle_path, &advanced_toml)?;
                return Ok(bundle_path.clone());
            }
            let disk = PathBuf::from(self.disk_path.to_string());
            self.build_machine(disk).save(bundle_path)?;
            return Ok(bundle_path.clone());
        }
        let dir = library::reserve_dir(&self.library_dir, &name)?;
        let bundle_path = dir.join(library::BUNDLE_FILE);
        if self.advanced {
            toml::from_str::<Machine>(&advanced_toml).map_err(std::io::Error::other)?;
            std::fs::write(&bundle_path, &advanced_toml)?;
            return Ok(bundle_path);
        }
        let disk_path = if self.existing_disk {
            PathBuf::from(self.disk_path.to_string())
        } else {
            let disk_path = dir.join("disk.qcow2");
            player::create_disk(&disk_path, self.disk_size_gb as u32)?;
            disk_path
        };
        self.build_machine(disk_path).save(&bundle_path)?;
        Ok(bundle_path)
    }
}

impl ffi::Wizard {
    fn open_fresh(self: Pin<&mut Self>) {
        let mut fresh = WizardRust::default();
        fresh.open = true;
        self.apply(fresh);
    }

    fn open_edit(mut self: Pin<&mut Self>, bundle_path: &QString) {
        let path = PathBuf::from(bundle_path.to_string());
        let machine = match Machine::load(&path) {
            Ok(m) => m,
            Err(e) => {
                self.as_mut().set_error(qs(format!("{}: {e}", path.display())));
                return;
            }
        };
        let original_toml = std::fs::read_to_string(&path).unwrap_or_default();
        let mut state = WizardRust {
            open: true,
            editing: true,
            title: QString::from("Edit machine"),
            family: match machine.family {
                Family::Win98 => 0,
                Family::Xp => 1,
            },
            name: qs(&machine.name),
            ram_mb: machine.ram_mb as i32,
            // An existing machine's memory is a chosen value whatever it
            // came from: changing family must not rewrite it.
            ram_chosen: true,
            accel: accel_index(machine.effective_accel()),
            accel_chosen: true,
            network: machine.network,
            existing_disk: true,
            disk_path: qs(machine.disk.display()),
            install_media: machine.boot_disc().map(|d| qs(d.display())).unwrap_or_default(),
            shader_profile: machine.shader_profile.as_deref().map(qs).unwrap_or_default(),
            edit_path: Some(path),
            edit_shader: machine.shader.clone(),
            edit_toml: original_toml,
            ..WizardRust::default()
        };
        state.recompute();
        self.apply(state);
    }

    fn choose_family(self: Pin<&mut Self>, family: i32) {
        let mut state = self.rust().clone();
        state.family = family;
        // Whatever nobody has chosen follows the family.
        let f = family_of(family);
        if !state.ram_chosen {
            state.ram_mb = bundle::default_ram_mb(f) as i32;
        }
        if !state.accel_chosen {
            state.accel = accel_index(bundle::default_accel(f));
        }
        state.recompute();
        self.apply(state);
    }

    fn choose_ram(self: Pin<&mut Self>, ram_mb: i32) {
        let mut state = self.rust().clone();
        state.ram_mb = ram_mb;
        state.ram_chosen = true;
        state.recompute();
        self.apply(state);
    }

    fn reset_ram(self: Pin<&mut Self>) {
        let mut state = self.rust().clone();
        state.ram_mb = bundle::default_ram_mb(family_of(state.family)) as i32;
        state.ram_chosen = false;
        state.recompute();
        self.apply(state);
    }

    fn choose_accel(self: Pin<&mut Self>, accel: i32) {
        let mut state = self.rust().clone();
        state.accel = accel;
        state.accel_chosen = true;
        state.recompute();
        self.apply(state);
    }

    fn reset_accel(self: Pin<&mut Self>) {
        let mut state = self.rust().clone();
        state.accel = accel_index(bundle::default_accel(family_of(state.family)));
        state.accel_chosen = false;
        state.recompute();
        self.apply(state);
    }

    fn choose_network(self: Pin<&mut Self>, network: bool) {
        let mut state = self.rust().clone();
        state.network = network;
        state.recompute();
        self.apply(state);
    }

    fn fill_advanced(mut self: Pin<&mut Self>) {
        if !self.advanced_toml.is_empty() {
            return;
        }
        let toml = self.rust().preview_toml();
        self.as_mut().set_advanced_toml(qs(toml));
    }

    fn submit(mut self: Pin<&mut Self>) -> bool {
        match self.rust().write() {
            Ok(path) => {
                self.as_mut().rust_mut().saved_path = path; // not a property
                self.as_mut().set_error(QString::default());
                self.as_mut().set_open(false);
                true
            }
            Err(e) => {
                self.as_mut().set_error(qs(e));
                false
            }
        }
    }

    fn saved_path(&self) -> QString {
        qs(self.rust().saved_path.display())
    }

    /// Write a whole new form state in, every Q_PROPERTY through its own
    /// generated setter so each one's notify fires for the values that
    /// actually moved. See this module's header for why a direct
    /// `rust_mut()` assignment here would be silent.
    fn apply(mut self: Pin<&mut Self>, state: WizardRust) {
        // The fields that are not properties: nothing observes them, so
        // they go straight in.
        {
            let mut this = self.as_mut().rust_mut();
            this.ram_chosen = state.ram_chosen;
            this.accel_chosen = state.accel_chosen;
            this.library_dir = state.library_dir.clone();
            this.edit_path = state.edit_path.clone();
            this.edit_shader = state.edit_shader.clone();
            this.edit_toml = state.edit_toml.clone();
            this.saved_path = state.saved_path.clone();
        }
        self.as_mut().set_open(state.open);
        self.as_mut().set_editing(state.editing);
        self.as_mut().set_title(state.title);
        self.as_mut().set_family(state.family);
        self.as_mut().set_name(state.name);
        self.as_mut().set_ram_mb(state.ram_mb);
        self.as_mut().set_ram_min(state.ram_min);
        self.as_mut().set_ram_max(state.ram_max);
        self.as_mut().set_ram_note(state.ram_note);
        self.as_mut().set_ram_is_default(state.ram_is_default);
        self.as_mut().set_accel(state.accel);
        self.as_mut().set_accel_note(state.accel_note);
        self.as_mut().set_accel_warning(state.accel_warning);
        self.as_mut().set_accel_is_default(state.accel_is_default);
        self.as_mut().set_network(state.network);
        self.as_mut().set_network_note(state.network_note);
        self.as_mut().set_existing_disk(state.existing_disk);
        self.as_mut().set_disk_path(state.disk_path);
        self.as_mut().set_disk_size_gb(state.disk_size_gb);
        self.as_mut().set_install_media(state.install_media);
        self.as_mut().set_shader_profile(state.shader_profile);
        self.as_mut().set_advanced(state.advanced);
        self.as_mut().set_advanced_toml(state.advanced_toml);
        self.as_mut().set_error(state.error);
    }
}
