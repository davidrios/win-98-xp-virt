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

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Machine {
    pub name: String,
    pub family: Family,
    pub ram_mb: u32,
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

impl Machine {
    /// A new machine from doc 06's reference defaults for `family`.
    pub fn reference(family: Family, name: String, disk: PathBuf) -> Self {
        let ram_mb = match family {
            Family::Win98 => 256, // doc 06: 256 MB default, ≤512 MB hard cap
            Family::Xp => 512,    // doc 06: 512 MB-1 GB default
        };
        Machine { name, family, ram_mb, disk, disc: None, discs: Vec::new(), shader_profile: None, shader: None }
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
    /// `pc_bios_dir` is `qemu/pc-bios` (see README's `-L`).
    pub fn qemu_args(&self, pc_bios_dir: &Path) -> Vec<String> {
        let mut args = vec![
            "-L".into(),
            pc_bios_dir.display().to_string(),
            "-machine".into(),
            "pc".into(),
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
        args.extend([
            "-drive".into(),
            drive,
            "-device".into(),
            format!("ide-cd,bus=ide.1,id={},drive=cd0,audiodev=embed0", crate::control::CDROM_ID),
        ]);
        args
    }
}
