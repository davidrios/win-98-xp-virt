//! The guided creation wizard (doc 07): family -> name -> disk size ->
//! install media -> a bundle written from doc 06's reference defaults.
//! An advanced toggle edits the raw TOML directly instead — still never
//! a QEMU command line, per doc 07. The same form doubles as the "Edit
//! machine" dialog for an existing bundle (`open_edit`); `submit` writes
//! back in place instead of reserving a new library directory.

use crate::bundle::{self, Accel, Boot, CpuSpeed, Family, Machine};
use crate::filepicker;
use crate::library;
use crate::player;
use std::path::{Path, PathBuf};

const DISK_FILTER: filepicker::Filter = ("Disk images", &["qcow2", "img", "raw"]);
use crate::disc_library::DISC_FILTER;
const FLOPPY_FILTER: filepicker::Filter = ("Floppy images", &["img", "ima", "vfd", "flp"]);

/// What editing an existing bundle needs to preserve: fields this form
/// doesn't expose (the raw shader override), so a quick edit can't
/// silently discard them. `original_toml` is the file's
/// exact current text, used as the advanced box's starting point instead
/// of a reconstruction — no information loss even for a field this form
/// (or a future one) doesn't model.
struct EditTarget {
    bundle_path: PathBuf,
    shader: Option<PathBuf>,
    original_toml: String,
}

pub struct Wizard {
    pub open: bool,
    family: Family,
    name: String,
    ram_mb: u32,
    /// Whether the RAM field holds a value someone chose. Until it does,
    /// switching family moves it to that family's own default (doc 06),
    /// which is what a user picking "XP" after "Win98" means; once they
    /// have set a number, a later family switch must not silently throw
    /// it away.
    ram_chosen: bool,
    accel: Accel,
    /// Same as `ram_chosen`, for the accelerator: until someone picks
    /// one, the family's own default follows the family (Win98 is
    /// emulated, XP is automatic — `bundle::default_accel`).
    accel_chosen: bool,
    /// Whether the machine gets a network adapter at all
    /// (`Machine::network`). Follows the family until someone touches
    /// it, like memory and the accelerator: DOS is the one family that
    /// doesn't get a card, and without this the form and
    /// `Machine::reference` would disagree about a new DOS machine —
    /// which they did, briefly, on 2026-09-06.
    network: bool,
    network_chosen: bool,
    /// The CPU the guest should feel like (`bundle::CpuSpeed`), with the
    /// same "until someone chooses, follow the family" rule as memory
    /// and acceleration — it is the field that makes a DOS machine a DOS
    /// machine, and switching family to DOS must bring it along.
    cpu_speed: CpuSpeed,
    cpu_speed_chosen: bool,
    /// A floppy image in A:, or empty for an empty drive.
    floppy: String,
    boot: Boot,
    existing_disk: bool,
    disk_path: String,
    disk_size_gb: u32,
    install_media: String,
    /// A shader profile id (`shader_library`), or `None` for the app
    /// default. Doesn't touch `EditTarget::shader` — the two overrides
    /// are independent (see `bundle::Machine`).
    shader_profile: Option<String>,
    advanced: bool,
    advanced_toml: String,
    error: Option<String>,
    editing: Option<EditTarget>,
}

impl Default for Wizard {
    fn default() -> Self {
        Wizard {
            open: false,
            family: Family::Win98,
            name: String::new(),
            ram_mb: bundle::default_ram_mb(Family::Win98),
            ram_chosen: false,
            accel: bundle::default_accel(Family::Win98),
            accel_chosen: false,
            network: bundle::default_network(Family::Win98),
            network_chosen: false,
            cpu_speed: bundle::default_cpu_speed(Family::Win98),
            cpu_speed_chosen: false,
            floppy: String::new(),
            boot: Boot::default(),
            existing_disk: false,
            disk_path: String::new(),
            disk_size_gb: 2,
            install_media: String::new(),
            shader_profile: None,
            advanced: false,
            advanced_toml: String::new(),
            error: None,
            editing: None,
        }
    }
}

impl Wizard {
    /// Reset to a fresh "New machine" form and open it.
    pub fn open_fresh(&mut self) {
        *self = Wizard { open: true, ..Default::default() };
    }

    /// The same, starting on a given family — the defaults that follow
    /// from it (memory) come with it.
    pub fn open_new(&mut self, family: Family) {
        self.open_fresh();
        self.family = family;
        self.ram_mb = bundle::default_ram_mb(family);
        self.accel = bundle::default_accel(family);
        self.cpu_speed = bundle::default_cpu_speed(family);
        self.network = bundle::default_network(family);
    }

    /// Open the form pre-filled from an existing bundle, to edit it in
    /// place instead of creating a new one.
    pub fn open_edit(&mut self, machine: &Machine, bundle_path: PathBuf) {
        let original_toml = std::fs::read_to_string(&bundle_path).unwrap_or_default();
        *self = Wizard {
            open: true,
            family: machine.family,
            name: machine.name.clone(),
            ram_mb: machine.ram_mb,
            // an existing machine's RAM is a chosen value, whatever it
            // came from: changing family must not rewrite it
            ram_chosen: true,
            accel: machine.effective_accel(),
            accel_chosen: true,
            network: machine.network,
            network_chosen: true,
            cpu_speed: machine.effective_cpu_speed(),
            cpu_speed_chosen: true,
            floppy: machine.floppy.as_ref().map(|f| f.display().to_string()).unwrap_or_default(),
            boot: machine.effective_boot(),
            existing_disk: true,
            disk_path: machine.disk.display().to_string(),
            install_media: machine.boot_disc().map(|d| d.display().to_string()).unwrap_or_default(),
            shader_profile: machine.shader_profile.clone(),
            editing: Some(EditTarget { bundle_path, shader: machine.shader.clone(), original_toml }),
            ..Default::default()
        };
    }

    /// Headless field access for scripted testing (`main.rs`'s
    /// `--wizard-edit`), so an edit's actual field change can be driven
    /// without a GUI click.
    pub fn set_name(&mut self, name: String) {
        self.name = name;
    }

    /// Same, for the memory and acceleration fields (`--wizard-edit`'s
    /// optional arguments): the values the widgets above would have set.
    pub fn set_ram_mb(&mut self, ram_mb: u32) {
        let range = bundle::ram_mb_range(self.family);
        self.ram_mb = ram_mb.clamp(*range.start(), *range.end());
        self.ram_chosen = true;
    }

    pub fn set_accel(&mut self, accel: Accel) {
        self.accel = accel;
        self.accel_chosen = true;
    }

    pub fn set_network(&mut self, network: bool) {
        self.network = network;
        self.network_chosen = true;
    }

    /// Headless construction (a debug verb; see `main.rs`'s `--wizard-new`)
    /// with the same fields the window's widgets would otherwise have set,
    /// so the wizard's actual disk-creation/save logic (`submit`, below)
    /// can be exercised without clicking through the GUI.
    pub fn with_new_disk(family: Family, name: String, disk_size_gb: u32) -> Wizard {
        Wizard {
            family,
            name,
            disk_size_gb,
            ram_mb: bundle::default_ram_mb(family),
            accel: bundle::default_accel(family),
            ..Default::default()
        }
    }

    /// Renders the wizard window if open. `shader_profiles` is the
    /// current shader-profile library (`shader_library::scan`), for the
    /// "Shader profile" picker. Returns the bundle's path once a machine
    /// has actually been created or saved, so the caller can rescan the
    /// library.
    pub fn show(
        &mut self,
        ctx: &egui::Context,
        library_dir: &Path,
        shader_profiles: &[crate::shader_library::ProfileEntry],
    ) -> Option<PathBuf> {
        if !self.open {
            return None;
        }
        let editing = self.editing.is_some();
        let mut done = None;
        let mut still_open = true;
        egui::Window::new(if editing { "Edit machine" } else { "New machine" })
            .open(&mut still_open)
            .collapsible(false)
            .resizable(false)
            .show(ctx, |ui| {
                let was = self.family;
                egui::ComboBox::from_label("Family")
                    .selected_text(self.family.label())
                    .show_ui(ui, |ui| {
                        for f in Family::ALL {
                            ui.selectable_value(&mut self.family, f, f.label());
                        }
                    });
                if self.family != was {
                    // whatever nobody has chosen follows the family
                    if !self.ram_chosen {
                        self.ram_mb = bundle::default_ram_mb(self.family);
                    }
                    if !self.accel_chosen {
                        self.accel = bundle::default_accel(self.family);
                    }
                    if !self.cpu_speed_chosen {
                        self.cpu_speed = bundle::default_cpu_speed(self.family);
                    }
                    if !self.network_chosen {
                        self.network = bundle::default_network(self.family);
                    }
                }
                ui.horizontal(|ui| {
                    ui.label("Name");
                    ui.text_edit_singleline(&mut self.name);
                });
                ui.separator();
                self.memory_ui(ui);
                self.cpu_speed_ui(ui);
                self.accel_ui(ui);
                self.network_ui(ui);
                ui.separator();
                if editing {
                    filepicker::path_field(ui, "Disk path", &mut self.disk_path, Some(DISK_FILTER));
                } else {
                    ui.checkbox(&mut self.existing_disk, "Use an existing disk image");
                    if self.existing_disk {
                        filepicker::path_field(ui, "Disk path", &mut self.disk_path, Some(DISK_FILTER));
                    } else {
                        ui.horizontal(|ui| {
                            ui.label("New disk size (GB)");
                            ui.add(egui::DragValue::new(&mut self.disk_size_gb).range(1..=128));
                        });
                    }
                }
                filepicker::path_field(ui, "Install media (optional)", &mut self.install_media, Some(DISC_FILTER));
                filepicker::path_field(ui, "Floppy (optional)", &mut self.floppy, Some(FLOPPY_FILTER));
                egui::ComboBox::from_label("Boot from")
                    .selected_text(self.boot.label())
                    .show_ui(ui, |ui| {
                        for b in Boot::ALL {
                            ui.selectable_value(&mut self.boot, b, b.label());
                        }
                    });
                if self.boot == Boot::Floppy && self.floppy.trim().is_empty() {
                    ui.small("No floppy image: the machine will fall through to the hard disk.");
                }
                ui.separator();
                egui::ComboBox::from_label("Shader profile")
                    .selected_text(
                        self.shader_profile
                            .as_deref()
                            .and_then(|id| shader_profiles.iter().find(|e| crate::shader_library::id_of(&e.path) == id))
                            .map(|e| e.profile.name.as_str())
                            .unwrap_or("(default)"),
                    )
                    .show_ui(ui, |ui| {
                        ui.selectable_value(&mut self.shader_profile, None, "(default)");
                        for entry in shader_profiles {
                            let id = crate::shader_library::id_of(&entry.path);
                            ui.selectable_value(&mut self.shader_profile, Some(id), &entry.profile.name);
                        }
                    });
                ui.separator();
                ui.checkbox(&mut self.advanced, "Advanced: edit machine.toml directly");
                if self.advanced {
                    if self.advanced_toml.is_empty() {
                        self.advanced_toml = self.preview_toml();
                    }
                    ui.add(
                        egui::TextEdit::multiline(&mut self.advanced_toml)
                            .code_editor()
                            .desired_rows(10),
                    );
                }
                if let Some(err) = &self.error {
                    ui.colored_label(egui::Color32::RED, err);
                }
                if ui.button(if editing { "Save" } else { "Create" }).clicked() {
                    match self.submit(library_dir) {
                        Ok(path) => {
                            done = Some(path);
                            self.error = None;
                        }
                        Err(e) => self.error = Some(e.to_string()),
                    }
                }
            });
        self.open = still_open && done.is_none();
        done
    }

    /// The RAM row. The range is per family (`bundle::ram_mb_range`), so
    /// the form cannot produce a Win98 machine with more memory than
    /// Win98 can boot with; a clamped value is corrected in place rather
    /// than refused at save time, and the reason is on screen next to it.
    fn memory_ui(&mut self, ui: &mut egui::Ui) {
        let range = bundle::ram_mb_range(self.family);
        let (min, max) = (*range.start(), *range.end());
        ui.horizontal(|ui| {
            ui.label("Memory (MB)");
            let r = ui.add(egui::DragValue::new(&mut self.ram_mb).speed(16.0).range(range.clone()));
            if r.changed() {
                self.ram_chosen = true;
            }
            if ui
                .add_enabled(
                    self.ram_mb != bundle::default_ram_mb(self.family),
                    egui::Button::new("Default"),
                )
                .clicked()
            {
                self.ram_mb = bundle::default_ram_mb(self.family);
                self.ram_chosen = false;
            }
        });
        self.ram_mb = self.ram_mb.clamp(min, max);
        if self.family == Family::Win98 && self.ram_mb >= max {
            ui.small("512 MB is Win98's ceiling (doc 06): more and it does not boot.");
        }
    }

    /// The acceleration row, plus what this host can actually do — the
    /// picker alone would leave "Automatic" meaning something invisible.
    /// The processor the guest should feel like. Named machines rather
    /// than a number, because "how many instructions per second" is not
    /// a thing anyone knows about their DOS game, while "it wants a 486"
    /// is written on the box.
    fn cpu_speed_ui(&mut self, ui: &mut egui::Ui) {
        let default = bundle::default_cpu_speed(self.family);
        ui.horizontal(|ui| {
            let was = self.cpu_speed;
            egui::ComboBox::from_label("Processor")
                .selected_text(self.cpu_speed.label())
                .show_ui(ui, |ui| {
                    for s in CpuSpeed::ALL {
                        ui.selectable_value(&mut self.cpu_speed, s, s.label());
                    }
                });
            if self.cpu_speed != was {
                self.cpu_speed_chosen = true;
            }
            if ui.add_enabled(self.cpu_speed != default, egui::Button::new("Default")).clicked() {
                self.cpu_speed = default;
                self.cpu_speed_chosen = false;
            }
        });
        if self.cpu_speed == CpuSpeed::Unthrottled {
            ui.small("Full speed. Right for Windows; most DOS software of the 486 era needs a slower one.");
        } else {
            // Both consequences, before the machine is created rather
            // than after it behaves oddly.
            ui.small("DOS-era software times itself against the CPU it finds, so this is what makes a game playable.");
            ui.small("A chosen processor means the machine is emulated: QEMU cannot slow a CPU down under KVM.");
        }
    }

    fn accel_ui(&mut self, ui: &mut egui::Ui) {
        let have_kvm = crate::player::kvm_available();
        let default = bundle::default_accel(self.family);
        ui.horizontal(|ui| {
            let was = self.accel;
            egui::ComboBox::from_label("Acceleration")
                .selected_text(match self.accel {
                    Accel::Auto => "Automatic",
                    Accel::Kvm => "KVM (required)",
                    Accel::Tcg => "Emulation",
                })
                .show_ui(ui, |ui| {
                    ui.selectable_value(&mut self.accel, Accel::Auto, "Automatic");
                    ui.selectable_value(&mut self.accel, Accel::Kvm, "KVM (required)");
                    ui.selectable_value(&mut self.accel, Accel::Tcg, "Emulation");
                });
            if self.accel != was {
                self.accel_chosen = true;
            }
            if ui.add_enabled(self.accel != default, egui::Button::new("Default")).clicked() {
                self.accel = default;
                self.accel_chosen = false;
            }
        });
        match (self.accel, have_kvm) {
            (Accel::Auto, true) => ui.small("KVM is available on this host and will be used."),
            (Accel::Auto, false) => ui.small("No KVM on this host: this machine will be emulated."),
            (Accel::Kvm, true) => ui.small("KVM is available on this host."),
            (Accel::Kvm, false) => ui.colored_label(
                egui::Color32::from_rgb(200, 140, 0),
                "No KVM on this host: this machine will refuse to start.",
            ),
            (Accel::Tcg, _) => ui.small("Emulated: the era-CPU behaviour everything here is tuned for."),
        };
        if self.family == Family::Win98 && self.accel != Accel::Tcg && have_kvm {
            ui.small("Win98 runs at host speed under KVM, which its own fast-CPU bugs dislike.");
        }
    }

    /// The networking row: one checkbox, because there is one question
    /// here — does this machine have a network card. What it gets when
    /// it does is QEMU's user-mode NAT (doc 06's per-family NIC), which
    /// needs no host privileges and gives nothing on the network a way
    /// in; the line under the box says so, since "networking" otherwise
    /// sounds like the guest is being put on the LAN.
    fn network_ui(&mut self, ui: &mut egui::Ui) {
        if ui.checkbox(&mut self.network, "Networking").changed() {
            self.network_chosen = true;
        }
        if self.network {
            ui.small("Outbound only, through the host (user-mode NAT): nothing on the network can reach the guest.");
            // Worth saying once, next to the switch: these guests stopped
            // getting security fixes over twenty years ago, and their own
            // browsers are the least safe thing on the machine.
            ui.small("These are unpatched systems — don't browse the web on one.");
        } else {
            ui.small("No network adapter at all: the guest won't see a card or ask for its driver.");
        }
    }

    /// The `Machine` the current field values describe, given the disk
    /// path to use (a fresh disk's path isn't known until it's created,
    /// so callers that might still need to do that pass it in rather
    /// than this reading `disk_path` itself). Shared by the advanced
    /// box's default and the non-advanced submit path so they can't
    /// silently disagree.
    fn build_machine(&self, disk: PathBuf) -> Machine {
        let mut machine = match &self.editing {
            Some(edit) => Machine {
                name: self.name.clone(),
                family: self.family,
                ram_mb: self.ram_mb,
                accel: Some(self.accel),
                network: self.network,
                disk,
                disc: None,
                discs: Vec::new(),
                shader_profile: None,
                shader: edit.shader.clone(),
                floppy: None,
                boot: None,
                cpu_speed: None,
            },
            None => Machine::reference(self.family, self.name.clone(), disk),
        };
        // the form owns these for a new machine too, where `reference`
        // has just filled in the family defaults. `ram_chosen` decides,
        // not the field's current contents: a constructor that set the
        // family without going through the combo box (`with_new_disk`,
        // the headless verb) never had the chance to move the default
        // along with it, and silently writing Win98's 256 MB into an XP
        // machine is exactly the bug that produced.
        machine.ram_mb =
            if self.ram_chosen { self.ram_mb } else { bundle::default_ram_mb(self.family) };
        // Written out explicitly either way: what the form showed is what
        // the machine gets, even when it is the family's own default.
        machine.accel =
            Some(if self.accel_chosen { self.accel } else { bundle::default_accel(self.family) });
        // Same rule as memory and the processor, and for the same
        // reason: `with_new_disk` (the headless verb) sets a family
        // without going through the combo box, so an unchosen field has
        // to be read from the family here rather than from the form.
        machine.network =
            if self.network_chosen { self.network } else { bundle::default_network(self.family) };
        machine.cpu_speed =
            Some(if self.cpu_speed_chosen { self.cpu_speed } else { bundle::default_cpu_speed(self.family) });
        machine.boot = Some(self.boot);
        machine.floppy = Some(self.floppy.trim()).filter(|f| !f.is_empty()).map(PathBuf::from);
        machine.shader_profile = self.shader_profile.clone();
        // The single slot this form has is the machine's *boot* disc;
        // everything else lives on the shared shelf (`disc_library.rs`),
        // which `submit` also adds this one to.
        machine.disc = Some(self.install_media.trim())
            .filter(|m| !m.is_empty())
            .map(PathBuf::from);
        machine
    }

    fn preview_toml(&self) -> String {
        if let Some(edit) = &self.editing {
            return edit.original_toml.clone();
        }
        let disk: PathBuf = if self.existing_disk { self.disk_path.clone().into() } else { "disk.qcow2".into() };
        toml::to_string_pretty(&self.build_machine(disk)).unwrap_or_default()
    }

    pub fn submit(&self, library_dir: &Path) -> std::io::Result<PathBuf> {
        if self.name.trim().is_empty() {
            return Err(std::io::Error::other("a name is required"));
        }
        if let Some(edit) = &self.editing {
            let bundle_path = edit.bundle_path.clone();
            if self.advanced {
                // Validate before writing: a bad hand-edit shouldn't
                // silently corrupt the library with an unreadable bundle.
                toml::from_str::<Machine>(&self.advanced_toml).map_err(std::io::Error::other)?;
                std::fs::write(&bundle_path, &self.advanced_toml)?;
                return Ok(bundle_path);
            }
            let disk = PathBuf::from(&self.disk_path);
            self.build_machine(disk).save(&bundle_path)?;
            return Ok(bundle_path);
        }
        let dir = library::reserve_dir(library_dir, &self.name)?;
        let bundle_path = dir.join(library::BUNDLE_FILE);
        if self.advanced {
            toml::from_str::<Machine>(&self.advanced_toml).map_err(std::io::Error::other)?;
            std::fs::write(&bundle_path, &self.advanced_toml)?;
            return Ok(bundle_path);
        }
        let disk_path = if self.existing_disk {
            PathBuf::from(&self.disk_path)
        } else {
            let disk_path = dir.join("disk.qcow2");
            player::create_disk(&disk_path, self.disk_size_gb)?;
            disk_path
        };
        self.build_machine(disk_path).save(&bundle_path)?;
        Ok(bundle_path)
    }
}
