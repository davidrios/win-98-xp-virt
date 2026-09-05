//! The machine bundle format (doc 07): a declarative `machine.toml` the
//! launcher reads and writes. "Hand-written bundles + the player binary is
//! a fully supported path" (doc 07) — `qemu_args` is the one place that
//! translates a bundle into a real `qemu-system-i386` command line; no
//! user-visible QEMU command line exists anywhere else.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Which reference guest configuration (doc 06) a machine is modeled on.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Family {
    Win98,
    Xp,
}

/// How the guest's instructions are executed. Kept in the bundle rather
/// than decided at spawn time, because it is a property of the machine a
/// user can want to pin: an era CPU under TCG is the reference behaviour
/// the whole project is tuned for (docs 13 and 16's x87/SSE fast paths
/// only exist there), while KVM is what makes an XP game playable on a
/// Linux host.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Accel {
    /// KVM when the host has it, emulation otherwise. QEMU itself picks,
    /// from the `kvm:tcg` list — no host probing here can be wrong.
    #[default]
    Auto,
    /// KVM only: the machine refuses to start without it, which is what
    /// "required" has to mean to be worth choosing over `Auto`.
    Kvm,
    /// Emulation only. The honest choice for Win98: KVM runs the guest at
    /// host speed, and Win9x has real fast-CPU bugs (doc 06) that the
    /// `pentium3` model does not protect against, since it is the *speed*
    /// that trips them.
    Tcg,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Machine {
    pub name: String,
    pub family: Family,
    pub ram_mb: u32,
    /// How to execute the guest. Defaults to `Auto` so bundles written
    /// before this field existed keep working (and pick up KVM).
    #[serde(default)]
    pub accel: Accel,
    /// Primary IDE hard disk (qcow2).
    pub disk: PathBuf,
    /// The disc in the CD-ROM drive when the machine boots, if any. Just
    /// one: the *collection* of discs is the shared shelf
    /// (`disc_library.rs`), not a per-machine list, and any other disc is
    /// swapped in at runtime through the monitor (`control.rs`).
    #[serde(default)]
    pub disc: Option<PathBuf>,
    /// Superseded by `disc` + the shared shelf. Read so bundles written
    /// before the shelf became shared still boot the disc they named
    /// (see `boot_disc`), and so their other entries can be imported
    /// (`DiscLibrary::import_legacy`); `save` drops it.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub discs: Vec<PathBuf>,
    /// A named shader profile (`shader_library`) to run this machine
    /// with, by id; `None` uses the app default. Takes precedence over
    /// `shader` when both are set.
    #[serde(default)]
    pub shader_profile: Option<String>,
    /// Raw shader preset override, bypassing the profile manager (doc 07
    /// settings taxonomy: the advanced/hand-written-bundle escape hatch).
    /// `None` uses the app default.
    #[serde(default)]
    pub shader: Option<PathBuf>,
}

/// doc 06's RAM default for a family.
pub fn default_ram_mb(family: Family) -> u32 {
    match family {
        Family::Win98 => 256, // doc 06: 256 MB default, ≤512 MB hard cap
        Family::Xp => 512,    // doc 06: 512 MB-1 GB default
    }
}

/// What the UI lets a user ask for. The Win98 ceiling is doc 06's hard
/// cap, not a guess: Win9x fails to boot with much more than 512 MB (its
/// VCACHE sizing overflows), so offering 2 GB there would only produce a
/// machine that does not start. XP's is the practical 32-bit limit,
/// below the 3.5 GB where PCI space starts eating into RAM.
pub fn ram_mb_range(family: Family) -> std::ops::RangeInclusive<u32> {
    match family {
        Family::Win98 => 32..=512,
        Family::Xp => 64..=3072,
    }
}

impl Machine {
    /// A new machine from doc 06's reference defaults for `family`.
    pub fn reference(family: Family, name: String, disk: PathBuf) -> Self {
        Machine {
            name,
            family,
            ram_mb: default_ram_mb(family),
            accel: Accel::default(),
            disk,
            disc: None,
            discs: Vec::new(),
            shader_profile: None,
            shader: None,
        }
    }

    /// The disc in the drive at boot: `disc`, or the first entry of a
    /// pre-shared-shelf bundle's `discs` (which is exactly what the old
    /// `qemu_args` attached).
    pub fn boot_disc(&self) -> Option<&PathBuf> {
        self.disc.as_ref().or_else(|| self.discs.first())
    }

    pub fn load(path: &Path) -> std::io::Result<Machine> {
        let text = std::fs::read_to_string(path)?;
        toml::from_str(&text).map_err(std::io::Error::other)
    }

    /// Writes the bundle in the current format, which also migrates a
    /// legacy one: the boot disc moves to `disc` and the old per-machine
    /// `discs` list is dropped. Its entries aren't lost — the library
    /// scan imports them onto the shared shelf
    /// (`DiscLibrary::import_legacy`) before anything here can rewrite a
    /// bundle.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let mut out = self.clone();
        out.disc = self.boot_disc().cloned();
        out.discs.clear();
        let text = toml::to_string_pretty(&out).map_err(std::io::Error::other)?;
        std::fs::write(path, text)
    }

    /// The `qemu-system-i386` arguments the player expects on its own
    /// command line (`player -- <these>`), per doc 06's reference tables.
    /// `pc_bios_dir` is `qemu/pc-bios` (see README's `-L`); `shelf`, when
    /// given, is the flat disc-shelf file the drive answers the in-guest
    /// `CDSHELF` program from (`cdshelf/cdshelf_proto.h`).
    /// The `accel=` list for `-machine`. `Auto` is expressed as QEMU's own
    /// fallback list rather than by probing `/dev/kvm` here: the answer a
    /// probe gives can still be wrong at spawn time (permissions, a
    /// module unloaded since), and QEMU's list already means exactly
    /// "KVM if you can, emulation otherwise". `kvm` is only offered where
    /// it exists at all — on macOS the name is not a registered
    /// accelerator, and listing it there would print a warning on every
    /// boot for nothing.
    fn accel_list(&self) -> &'static str {
        match self.accel {
            Accel::Auto if cfg!(target_os = "linux") => "kvm:tcg",
            Accel::Auto | Accel::Tcg => "tcg",
            Accel::Kvm => "kvm",
        }
    }

    pub fn qemu_args(&self, pc_bios_dir: &Path, shelf: Option<&Path>) -> Vec<String> {
        let mut args = vec![
            "-L".into(),
            pc_bios_dir.display().to_string(),
            "-machine".into(),
            format!("pc,accel={}", self.accel_list()),
            "-m".into(),
            self.ram_mb.to_string(),
            // doc 06's floor for both families: avoids the fast-CPU Win9x
            // bugs and CPUID-dispatched guest code that mis-decodes under
            // -cpu host (the Max Payne JPEG decoder gotcha)
            "-cpu".into(),
            "pentium3".into(),
            "-drive".into(),
            format!("file={},if=ide,index=0,media=disk", self.disk.display()),
            "-usb".into(),
            "-device".into(),
            "usb-tablet".into(),
        ];
        match self.family {
            Family::Win98 => {
                args.extend(["-vga".into(), "cirrus".into()]);
                args.extend(["-netdev".into(), "user,id=n0".into()]);
                args.extend(["-device".into(), "pcnet,netdev=n0".into()]);
                args.extend(["-device".into(), "sb16,audiodev=embed0".into()]);
            }
            Family::Xp => {
                args.extend(["-vga".into(), "none".into()]);
                args.extend(["-device".into(), "d3dpt-vga".into()]);
                args.extend(["-netdev".into(), "user,id=n0".into()]);
                args.extend(["-device".into(), "rtl8139,netdev=n0".into()]);
                args.extend(["-device".into(), "AC97,audiodev=embed0".into()]);
            }
        }
        // The CD-ROM drive is always attached, empty tray and all: a
        // real machine of the era has one, and the launcher's live disc
        // swap (`control.rs`) needs a device to put a disc *into* — a
        // drive that only exists when the bundle happened to ship a disc
        // couldn't be loaded later. The id is what a medium change
        // addresses (`control::CDROM_ID`).
        let mut drive = "if=none,id=cd0,media=cdrom".to_string();
        if let Some(disc) = self.boot_disc() {
            drive.push_str(&format!(",file={}", disc.display()));
        }
        let mut cd = format!("ide-cd,bus=ide.1,id={},drive=cd0,audiodev=embed0", crate::control::CDROM_ID);
        if let Some(shelf) = shelf {
            // The drive answers the in-guest CDSHELF program from this
            // file (patch 52). Without it the vendor command reports
            // "no shelf" and the drive is an ordinary CD-ROM — which is
            // also what a hand-written bundle run straight through
            // `player` gets.
            cd.push_str(&format!(",shelf={}", shelf.display()));
        }
        args.extend(["-drive".into(), drive, "-device".into(), cd]);
        args
    }
}
