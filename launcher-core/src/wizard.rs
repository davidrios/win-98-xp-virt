//! The guided creation form (doc 07): family → name → memory →
//! processor → acceleration → networking → disk → install media → a
//! bundle written from doc 06's reference defaults. The same form edits
//! an existing bundle (`open_edit`), where `submit` writes back in place
//! instead of reserving a new library directory. An advanced toggle
//! edits the raw TOML instead — still never a QEMU command line.
//!
//! **Everything except the widgets is here**, including the sentences.
//! A front end reads `ram_note()`, `accel_note()`, `network_notes()` and
//! prints them; it does not compose its own. The Qt port used to, and
//! the two builds ended up telling the user different things about the
//! same checkbox ("Windows won't see a card" against "the guest won't
//! see a card"), which is a small symptom of the real problem: it also
//! had no processor, floppy or boot field at all, and its networking
//! checkbox didn't follow the family the way memory and the accelerator
//! do.
//!
//! The one asymmetry a shared form has to respect: **an immediate-mode
//! front end reads a widget's new value and compares it to the old one
//! in the same frame** (`if self.family != was { … }`), while a
//! retained-mode one has a property setter and no before/after pair. So
//! a field with a *consequence* — family, memory, acceleration,
//! networking, the processor — is private, with a `choose_*` that
//! applies the consequence and a `reset_*` that puts it back on the
//! family's default. The plain fields (a name, a path, a checkbox with
//! nothing behind it) are public and either front end writes them
//! directly.

use crate::browse::Filter;
use crate::bundle::{self, Accel, Boot, CpuSpeed, Family, Machine, Optimization, Optimizations};
use crate::disc_library::DISC_FILTER;
use crate::{host_gpu, library, player};
use std::path::{Path, PathBuf};

pub const DISK_FILTER: Filter<'static> = ("Disk images", &["qcow2", "img", "raw"]);
pub const FLOPPY_FILTER: Filter<'static> = ("Floppy images", &["img", "ima", "vfd", "flp"]);
/// Re-exported so a front end drawing this form needs one import for its
/// three file fields; the constant itself belongs to the shelf.
pub const MEDIA_FILTER: Filter<'static> = DISC_FILTER;

/// What editing an existing bundle needs to preserve: the fields this
/// form doesn't expose, so a quick edit can't silently discard them.
/// `original_toml` is the file's exact current text, used as the
/// advanced box's starting point instead of a reconstruction — no
/// information loss even for a field a future form doesn't model.
struct EditTarget {
    bundle_path: PathBuf,
    shader: Option<PathBuf>,
    original_toml: String,
}

/// The acceleration hint under the picker, and whether it is a warning
/// (a machine that will refuse to start) rather than a note.
pub struct AccelNote {
    pub text: String,
    pub warning: bool,
}

pub struct Form {
    /// Whether the window is up. A front end owns the window; this is
    /// the model's own answer to "should it be".
    pub open: bool,
    pub name: String,
    /// A floppy image in A:, or empty for an empty drive.
    pub floppy: String,
    pub boot: Boot,
    pub existing_disk: bool,
    pub disk_path: String,
    pub disk_size_gb: u32,
    pub install_media: String,
    /// A shader profile id (`shader_library`), or `None` for the app
    /// default. Independent of `EditTarget::shader` (see `bundle::Machine`).
    pub shader_profile: Option<String>,
    pub advanced: bool,
    pub advanced_toml: String,
    /// What the last `submit` failed with, for the form to show.
    pub error: Option<String>,
    /// The bundle the last successful `submit` wrote.
    saved_path: Option<PathBuf>,

    family: Family,
    ram_mb: u32,
    /// Whether the memory field holds a value someone chose. Until it
    /// does, switching family moves it to that family's own default (doc
    /// 06), which is what picking "XP" after "Win98" means; once a
    /// number has been set, a later family switch must not throw it away.
    ram_chosen: bool,
    accel: Accel,
    /// Same as `ram_chosen`, for the accelerator: until someone picks
    /// one, the family's own default follows the family (Win98 is
    /// emulated, XP is automatic — `bundle::default_accel`).
    accel_chosen: bool,
    /// Whether the machine gets a network adapter at all. Follows the
    /// family until someone touches it, like memory and the accelerator:
    /// DOS is the one family that doesn't get a card, and without this
    /// the form and `Machine::reference` disagree about a new DOS
    /// machine — which they did, briefly, on 2026-09-06.
    network: bool,
    network_chosen: bool,
    /// The CPU the guest should feel like, with the same rule — it is
    /// the field that makes a DOS machine a DOS machine, so switching
    /// family to DOS must bring it along.
    cpu_speed: CpuSpeed,
    cpu_speed_chosen: bool,
    /// Which of our own emulator fast paths this machine runs with.
    /// Private like the fields above, though nothing here follows the
    /// family: a checkbox that is *off* is a diagnosis someone is in the
    /// middle of, and `choose_optimization` is the only way to set one,
    /// which is what keeps the "only the difference is stored" rule
    /// (`Optimizations::set`) out of two front ends.
    optimizations: Optimizations,

    /// Whether this host can give a guest KVM. Read once when the form
    /// opens rather than per frame: it opens `/dev/kvm` to find out (a
    /// bare `exists()` misses the "not in the `kvm` group" case), and
    /// the answer cannot change while a form is on screen.
    have_kvm: bool,
    /// The same, for the guest's 3D (`host_gpu`, ADR-013), and for the
    /// same reason twice over: a probe is a whole `VkInstance`, and the
    /// answer cannot change while a form is on screen. `cached` so that
    /// every window opened in one session pays for it once.
    host_gpu: host_gpu::HostGpu,
    editing: Option<EditTarget>,
}

impl Default for Form {
    fn default() -> Self {
        Form {
            open: false,
            name: String::new(),
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
            saved_path: None,
            family: Family::Win98,
            ram_mb: bundle::default_ram_mb(Family::Win98),
            ram_chosen: false,
            accel: bundle::default_accel(Family::Win98),
            accel_chosen: false,
            network: bundle::default_network(Family::Win98),
            network_chosen: false,
            cpu_speed: bundle::default_cpu_speed(Family::Win98),
            cpu_speed_chosen: false,
            optimizations: Optimizations::default(),
            have_kvm: player::hw_accel_available(),
            host_gpu: host_gpu::cached().gpu,
            editing: None,
        }
    }
}

// --- opening -------------------------------------------------------

impl Form {
    /// Reset to a fresh "New machine" form and open it.
    pub fn open_fresh(&mut self) {
        *self = Form { open: true, ..Default::default() };
    }

    /// The same, starting on a given family — everything that follows
    /// from it (memory, the accelerator, the processor, the NIC) comes
    /// with it.
    pub fn open_new(&mut self, family: Family) {
        self.open_fresh();
        self.choose_family(family);
    }

    /// Open the form pre-filled from an existing bundle, to edit it in
    /// place instead of creating a new one.
    pub fn open_edit(&mut self, machine: &Machine, bundle_path: PathBuf) {
        let original_toml = std::fs::read_to_string(&bundle_path).unwrap_or_default();
        *self = Form {
            open: true,
            name: machine.name.clone(),
            family: machine.family,
            ram_mb: machine.ram_mb,
            // An existing machine's memory is a chosen value, whatever
            // it came from: changing family must not rewrite it. Same
            // for the other three.
            ram_chosen: true,
            accel: machine.effective_accel(),
            accel_chosen: true,
            network: machine.network,
            network_chosen: true,
            cpu_speed: machine.effective_cpu_speed(),
            cpu_speed_chosen: true,
            optimizations: machine.optimizations.clone(),
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

    /// The same, from a bundle path — what a front end that addresses
    /// windows by path (the Qt one does; every window there re-reads the
    /// bundle rather than being handed a copy) needs. The load error, if
    /// any, lands in `error` and the form does not open.
    pub fn open_edit_path(&mut self, bundle_path: PathBuf) {
        match Machine::load(&bundle_path) {
            Ok(machine) => self.open_edit(&machine, bundle_path),
            Err(e) => {
                self.open = false;
                self.error = Some(format!("{}: {e}", bundle_path.display()));
            }
        }
    }

    /// Headless construction with the same fields the widgets would have
    /// set, so `submit`'s real disk-creation and save logic can be
    /// exercised without a GUI click (`cli`'s `--wizard-new`).
    pub fn with_new_disk(family: Family, name: String, disk_size_gb: u32) -> Form {
        let mut form = Form { name, disk_size_gb, ..Default::default() };
        form.choose_family(family);
        form
    }

    pub fn is_editing(&self) -> bool {
        self.editing.is_some()
    }

    /// "Edit machine" or "New machine" — the window's own title.
    pub fn title(&self) -> &'static str {
        if self.is_editing() {
            "Edit machine"
        } else {
            "New machine"
        }
    }
}

// --- the fields with a consequence ----------------------------------

impl Form {
    pub fn family(&self) -> Family {
        self.family
    }

    /// Pick the family, moving whatever nobody has chosen to that
    /// family's own default with it.
    pub fn choose_family(&mut self, family: Family) {
        self.family = family;
        if !self.ram_chosen {
            self.ram_mb = bundle::default_ram_mb(family);
        }
        if !self.accel_chosen {
            self.accel = bundle::default_accel(family);
        }
        if !self.cpu_speed_chosen {
            self.cpu_speed = bundle::default_cpu_speed(family);
        }
        if !self.network_chosen {
            self.network = bundle::default_network(family);
        }
        // The new family's ceiling may be below the memory already in
        // the field (Win98 stops at 512 MB), so the clamp is part of the
        // switch rather than something the drawing code remembers to do.
        self.ram_mb = self.ram_mb.clamp(*self.ram_range().start(), *self.ram_range().end());
    }

    pub fn ram_mb(&self) -> u32 {
        self.ram_mb
    }

    pub fn ram_range(&self) -> std::ops::RangeInclusive<u32> {
        bundle::ram_mb_range(self.family)
    }

    pub fn ram_is_default(&self) -> bool {
        self.ram_mb == bundle::default_ram_mb(self.family)
    }

    /// Set the memory, clamped to the family's range — a value outside
    /// it is corrected here rather than refused at save time, so the
    /// form cannot produce a Win98 machine with more memory than Win98
    /// can boot with.
    pub fn choose_ram_mb(&mut self, ram_mb: u32) {
        let range = self.ram_range();
        self.ram_mb = ram_mb.clamp(*range.start(), *range.end());
        self.ram_chosen = true;
    }

    pub fn reset_ram(&mut self) {
        self.ram_mb = bundle::default_ram_mb(self.family);
        self.ram_chosen = false;
    }

    pub fn ram_note(&self) -> Option<&'static str> {
        (self.family == Family::Win98 && self.ram_mb >= *self.ram_range().end())
            .then_some("512 MB is Win98's ceiling (doc 06): more and it does not boot.")
    }

    pub fn cpu_speed(&self) -> CpuSpeed {
        self.cpu_speed
    }

    pub fn cpu_speed_is_default(&self) -> bool {
        self.cpu_speed == bundle::default_cpu_speed(self.family)
    }

    pub fn choose_cpu_speed(&mut self, cpu_speed: CpuSpeed) {
        self.cpu_speed = cpu_speed;
        self.cpu_speed_chosen = true;
    }

    pub fn reset_cpu_speed(&mut self) {
        self.cpu_speed = bundle::default_cpu_speed(self.family);
        self.cpu_speed_chosen = false;
    }

    /// What follows from the chosen processor, said before the machine
    /// is created rather than after it behaves oddly.
    pub fn cpu_speed_notes(&self) -> &'static [&'static str] {
        if self.cpu_speed == CpuSpeed::Unthrottled {
            &["Full speed. Right for Windows; most DOS software of the 486 era needs a slower one."]
        } else {
            &[
                "DOS-era software times itself against the CPU it finds, so this is what makes a game playable.",
                "A chosen processor means the machine is emulated: QEMU cannot slow a CPU down under KVM.",
            ]
        }
    }

    pub fn accel(&self) -> Accel {
        self.accel
    }

    pub fn accel_is_default(&self) -> bool {
        self.accel == bundle::default_accel(self.family)
    }

    pub fn choose_accel(&mut self, accel: Accel) {
        self.accel = accel;
        self.accel_chosen = true;
    }

    pub fn reset_accel(&mut self) {
        self.accel = bundle::default_accel(self.family);
        self.accel_chosen = false;
    }

    /// Whether this host has hardware acceleration — the picker alone
    /// would leave "Automatic" meaning something invisible.
    pub fn have_kvm(&self) -> bool {
        self.have_kvm
    }

    pub fn accel_note(&self) -> AccelNote {
        // KVM on Linux, WHPX on Windows: the note names what this host
        // has, because "No KVM on this host" on a Windows machine is
        // both wrong and unactionable.
        let hw = player::hw_accel_label().unwrap_or("Hardware acceleration");
        let mut text = match (self.accel, self.have_kvm) {
            (Accel::Auto, true) => format!("{hw} is available on this host and will be used."),
            (Accel::Auto, false) => format!("No {hw} on this host: this machine will be emulated."),
            (Accel::Kvm, true) => format!("{hw} is available on this host."),
            (Accel::Kvm, false) => format!("No {hw} on this host: this machine will refuse to start."),
            (Accel::Tcg, _) => "Emulated: the era-CPU behaviour everything here is tuned for.".to_string(),
        };
        if self.family == Family::Win98 && self.accel != Accel::Tcg && self.have_kvm {
            text.push('\n');
            text.push_str(&format!("Win98 runs at host speed under {hw}, which its own fast-CPU bugs dislike."));
        }
        AccelNote { text, warning: matches!((self.accel, self.have_kvm), (Accel::Kvm, false)) }
    }

    /// What this host will give the guest's 3D, under the acceleration
    /// row (ADR-013). Not a picker, because there is nothing to pick: the
    /// host settles it, and the only failure worth preventing is finding
    /// out after the machine exists. A DOS machine has no Direct3D to
    /// place and gets no line.
    ///
    /// `warning` is true only for a software Vulkan driver — the
    /// executor does run there, and slowly, which is the one case where
    /// what the user sees will disappoint them. A host with no Vulkan at
    /// all is a plain note: it runs every machine, through the OpenGL
    /// pass-through with WineD3D in the guest, and nothing is wrong.
    pub fn graphics_note(&self) -> Option<AccelNote> {
        if self.family == Family::Dos {
            return None;
        }
        let mut text = format!("3D: {}", self.host_gpu.headline());
        if let Some(advice) = self.host_gpu.advice() {
            text.push('\n');
            text.push_str(advice);
        }
        Some(AccelNote { text, warning: self.host_gpu.is_slow() })
    }

    pub fn network(&self) -> bool {
        self.network
    }

    pub fn choose_network(&mut self, network: bool) {
        self.network = network;
        self.network_chosen = true;
    }

    /// One checkbox, because there is one question: does this machine
    /// have a network card. What it gets when it does is QEMU's
    /// user-mode NAT (doc 06's per-family NIC), which needs no host
    /// privileges and gives nothing on the network a way in — worth
    /// saying, since "networking" otherwise sounds like the guest is
    /// being put on the LAN. And worth saying once, next to the switch,
    /// that these guests stopped getting security fixes twenty years ago.
    pub fn network_notes(&self) -> &'static [&'static str] {
        if self.network {
            &[
                "Outbound only, through the host (user-mode NAT): nothing on the network can reach the guest.",
                "These are unpatched systems — don't browse the web on one.",
            ]
        } else {
            &["No network adapter at all: the guest won't see a card or ask for its driver."]
        }
    }

    pub fn optimizations(&self) -> &Optimizations {
        &self.optimizations
    }

    pub fn optimization_enabled(&self, opt: Optimization) -> bool {
        self.optimizations.enabled(opt)
    }

    pub fn choose_optimization(&mut self, opt: Optimization, on: bool) {
        self.optimizations.set(opt, on);
    }

    /// Every fast path back on its shipped setting — the way out of a
    /// diagnosis session, and the only button in that section that
    /// someone will look for.
    pub fn reset_optimizations(&mut self) {
        self.optimizations.reset();
    }

    pub fn optimizations_are_default(&self) -> bool {
        self.optimizations.all_default()
    }

    /// What the collapsed section says about itself, so a machine with
    /// something turned off says so without being opened.
    pub fn optimizations_summary(&self) -> String {
        self.optimizations.summary()
    }

    /// The one thing worth saying above the switches: on a machine that
    /// is about to run on KVM they are all inert, because the guest's
    /// instructions are then executed by the host CPU and there is no
    /// emulator in the path to have a fast path.
    pub fn optimizations_note(&self) -> &'static str {
        if self.will_use_kvm() {
            "This machine runs on KVM, where none of these apply: they are fast paths in the emulator. \
             Choose Emulation above (or a processor, which forces it) to use them."
        } else {
            "Our own additions to QEMU, each measured (patches/qemu/README.md). Turn one off to find out \
             whether it is what makes a guest compute the wrong number or stop drawing."
        }
    }

    /// Whether this machine, as the form currently stands, will actually
    /// run on KVM: what `effective_accel` decides, plus what this host
    /// has — `Automatic` on a box without `/dev/kvm` is emulation.
    fn will_use_kvm(&self) -> bool {
        self.cpu_speed.icount_shift().is_none()
            && match self.accel {
                Accel::Kvm => true,
                Accel::Auto => self.have_kvm,
                Accel::Tcg => false,
            }
    }

    /// The one thing the boot picker can say that isn't obvious: a
    /// machine told to boot from a floppy it hasn't got.
    pub fn boot_note(&self) -> Option<&'static str> {
        (self.boot == Boot::Floppy && self.floppy.trim().is_empty())
            .then_some("No floppy image: the machine will fall through to the hard disk.")
    }
}

// --- what it writes -------------------------------------------------

impl Form {
    /// The `Machine` the current field values describe, given the disk
    /// path to use (a fresh disk's path isn't known until it's created,
    /// so callers that might still need to do that pass it in). Shared
    /// by the advanced box's default and the plain submit path, so the
    /// two cannot silently disagree.
    pub fn build_machine(&self, disk: PathBuf) -> Machine {
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
                optimizations: Optimizations::default(),
            },
            None => Machine::reference(self.family, self.name.clone(), disk),
        };
        // The form owns these for a new machine too, where `reference`
        // has just filled in the family defaults. The `*_chosen` flag
        // decides, not the field's current contents: a form constructed
        // without going through the family picker (`with_new_disk`, the
        // headless verb) never had the chance to move the default along
        // with it, and silently writing Win98's 256 MB into an XP
        // machine is exactly the bug that produced.
        machine.ram_mb = if self.ram_chosen { self.ram_mb } else { bundle::default_ram_mb(self.family) };
        // Written out explicitly either way: what the form showed is
        // what the machine gets, even when it is the family's default.
        machine.accel = Some(if self.accel_chosen { self.accel } else { bundle::default_accel(self.family) });
        machine.network = if self.network_chosen { self.network } else { bundle::default_network(self.family) };
        machine.cpu_speed =
            Some(if self.cpu_speed_chosen { self.cpu_speed } else { bundle::default_cpu_speed(self.family) });
        // Only what someone turned off is in here, so this is a clone
        // of "nothing" for almost every machine (`Optimizations`).
        machine.optimizations = self.optimizations.clone();
        machine.boot = Some(self.boot);
        machine.floppy = Some(self.floppy.trim()).filter(|f| !f.is_empty()).map(PathBuf::from);
        machine.shader_profile = self.shader_profile.clone();
        // The single slot this form has is the machine's *boot* disc;
        // everything else lives on the shared shelf (`disc_library.rs`).
        machine.disc = Some(self.install_media.trim()).filter(|m| !m.is_empty()).map(PathBuf::from);
        machine
    }

    /// What the advanced box opens on: the file's exact current text
    /// when editing, the TOML this form describes when creating.
    pub fn preview_toml(&self) -> String {
        if let Some(edit) = &self.editing {
            return edit.original_toml.clone();
        }
        let disk: PathBuf =
            if self.existing_disk { self.disk_path.clone().into() } else { "disk.qcow2".into() };
        toml::to_string_pretty(&self.build_machine(disk)).unwrap_or_default()
    }

    /// Fill the advanced box if it is still empty. Called when the
    /// toggle goes on: an immediate-mode front end does it while
    /// drawing, a retained-mode one from the checkbox's handler.
    pub fn fill_advanced(&mut self) {
        if self.advanced_toml.is_empty() {
            self.advanced_toml = self.preview_toml();
        }
    }

    /// Create or save the machine, closing the form on success. `None`
    /// leaves it open with `error` saying why.
    pub fn submit(&mut self, library_dir: &Path) -> Option<PathBuf> {
        match self.write(library_dir) {
            Ok(path) => {
                self.error = None;
                self.saved_path = Some(path.clone());
                self.open = false;
                Some(path)
            }
            Err(e) => {
                self.error = Some(e.to_string());
                None
            }
        }
    }

    /// The `machine.toml` the last successful `submit` wrote.
    pub fn saved_path(&self) -> Option<&Path> {
        self.saved_path.as_deref()
    }

    fn write(&self, library_dir: &Path) -> std::io::Result<PathBuf> {
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
            self.build_machine(PathBuf::from(&self.disk_path)).save(&bundle_path)?;
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
