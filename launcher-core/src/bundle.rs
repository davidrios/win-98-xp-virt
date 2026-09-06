//! The machine bundle format (doc 07): a declarative `machine.toml` the
//! launcher reads and writes. "Hand-written bundles + the player binary is
//! a fully supported path" (doc 07) — `qemu_args` is the one place that
//! translates a bundle into a real `qemu-system-i386` command line; no
//! user-visible QEMU command line exists anywhere else.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Which reference guest configuration (doc 06) a machine is modeled on.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Family {
    Win98,
    Xp,
    /// A DOS machine: MS-DOS or FreeDOS on the same i440FX PC, with the
    /// SB16 the Win98 family already carries "for DOS boxes/games" (doc
    /// 06) and no network card. What actually makes it a *DOS* machine
    /// is `CpuSpeed`: the era's software paces itself by how fast the
    /// CPU is, and emulation is far too fast for it (doc 06).
    Dos,
}

impl Family {
    /// In the order a picker should offer them, newest first.
    pub const ALL: [Family; 3] = [Family::Win98, Family::Xp, Family::Dos];

    pub fn label(self) -> &'static str {
        match self {
            Family::Win98 => "Win98",
            Family::Xp => "XP",
            Family::Dos => "DOS",
        }
    }
}

/// How fast the guest's CPU is allowed to be, named after the machine it
/// feels like rather than after the knob underneath.
///
/// DOS-era software calibrates delay loops against the CPU it finds and
/// then trusts the answer forever, so on a fast machine it does not merely
/// run quickly — it runs *wrong*: unplayable games, Turbo Pascal's
/// "runtime error 200", music that plays at double speed. Our TCG runs a
/// DOS guest at around 610 million instructions/s on the Linux box
/// (measured 2026-09-06, a tight loop under `-cpu pentium3`), which is
/// Pentium III territory; KVM is far beyond that.
///
/// QEMU's only rate control is `-icount`, whose `shift` sets one
/// instruction per 2^shift ns — so the rates below are powers of two by
/// construction, and the labels say which real machine each is closest
/// to. Two things follow from how it works, both of which the UI says
/// out loud: it needs `align=on` to pace against the host at all (without
/// it the guest only *believes* it is slow), and it cannot coexist with
/// KVM, so a throttled machine runs emulated whatever its `accel` says.
///
/// The cap is exact where it matters. Measured on the Linux box with a
/// 100M-instruction loop: 30.6 MIPS asked 31.25, 7.9 asked 7.8. Above
/// ~30 MIPS the alignment only corrects the guest when it falls *behind*,
/// so the fast entries are a ceiling the host may not reach and may
/// overshoot — which is why the two slowest entries are the ones a 1993
/// game should be given.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum CpuSpeed {
    /// No throttle: as fast as this host emulates (or KVM, if the machine
    /// asked for it). Right for Windows, wrong for most DOS software.
    #[default]
    #[serde(rename = "unthrottled")]
    Unthrottled,
    #[serde(rename = "pentium-133")]
    Pentium133,
    #[serde(rename = "pentium-75")]
    Pentium75,
    #[serde(rename = "486dx2-66")]
    Dx266,
    #[serde(rename = "486sx-25")]
    Sx25,
    #[serde(rename = "386dx-33")]
    Dx33,
    #[serde(rename = "286-12")]
    At286,
}

impl CpuSpeed {
    /// In the order a combo box should offer them: fastest first, because
    /// "as fast as possible" is the answer for everything that is not a
    /// DOS game, and the list then reads downwards through the eras.
    pub const ALL: [CpuSpeed; 7] = [
        CpuSpeed::Unthrottled,
        CpuSpeed::Pentium133,
        CpuSpeed::Pentium75,
        CpuSpeed::Dx266,
        CpuSpeed::Sx25,
        CpuSpeed::Dx33,
        CpuSpeed::At286,
    ];

    /// `-icount shift=`, or `None` for no throttle at all.
    pub fn icount_shift(self) -> Option<u32> {
        match self {
            CpuSpeed::Unthrottled => None,
            CpuSpeed::Pentium133 => Some(3), // 125 M instructions/s
            CpuSpeed::Pentium75 => Some(4),  // 62.5
            CpuSpeed::Dx266 => Some(5),      // 31.25
            CpuSpeed::Sx25 => Some(6),       // 15.6
            CpuSpeed::Dx33 => Some(7),       // 7.8
            CpuSpeed::At286 => Some(8),      // 3.9
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            CpuSpeed::Unthrottled => "Unthrottled (as fast as the host emulates)",
            CpuSpeed::Pentium133 => "Pentium 133 (~125 M instructions/s)",
            CpuSpeed::Pentium75 => "Pentium 75 (~62 M)",
            CpuSpeed::Dx266 => "486DX2-66 (~31 M)",
            CpuSpeed::Sx25 => "486SX-25 (~16 M)",
            CpuSpeed::Dx33 => "386DX-33 (~8 M)",
            CpuSpeed::At286 => "286-12 (~4 M)",
        }
    }
}

/// Which drive the machine boots from. `Auto` leaves the order to QEMU,
/// which tries the hard disk, then the floppy, then the CD — the right
/// answer for an installed Windows and for the wizard's "boot the
/// installer from the CD because the new disk is blank" case alike.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Boot {
    #[default]
    Auto,
    Disk,
    /// The floppy first, the disk if there is no disk in it: a DOS boot
    /// disk, or a Windows machine being repaired from one.
    Floppy,
    /// The CD first: reinstalling over a disk that still boots.
    Cd,
}

impl Boot {
    pub const ALL: [Boot; 4] = [Boot::Auto, Boot::Disk, Boot::Floppy, Boot::Cd];

    pub fn label(self) -> &'static str {
        match self {
            Boot::Auto => "Automatic",
            Boot::Disk => "Hard disk",
            Boot::Floppy => "Floppy, then hard disk",
            Boot::Cd => "CD, then hard disk",
        }
    }

    fn order(self) -> Option<&'static str> {
        match self {
            Boot::Auto => None,
            Boot::Disk => Some("c"),
            Boot::Floppy => Some("ac"),
            Boot::Cd => Some("dc"),
        }
    }
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
    /// Hardware acceleration when the host has it, emulation otherwise.
    /// QEMU itself picks, from the `kvm:tcg` (Windows: `whpx:tcg`) list —
    /// no host probing here can be wrong.
    #[default]
    Auto,
    /// Hardware acceleration only: the machine refuses to start without
    /// it, which is what "required" has to mean to be worth choosing over
    /// `Auto`. Named `kvm` in the bundle for every host — the field says
    /// what the user asked for, and each host spells it its own way
    /// (`whpx` on Windows), so a machine directory copied between them
    /// keeps meaning the same thing.
    Kvm,
    /// Emulation only. The honest choice for Win98: KVM runs the guest at
    /// host speed, and Win9x has real fast-CPU bugs (doc 06) that the
    /// `pentium3` model does not protect against, since it is the *speed*
    /// that trips them.
    Tcg,
}

impl Accel {
    pub const ALL: [Accel; 3] = [Accel::Auto, Accel::Kvm, Accel::Tcg];

    /// The label both front ends show. Here rather than inline in a
    /// combo box, like `Family::label` and `Boot::label`, so the two
    /// cannot end up offering differently-worded choices.
    pub fn label(self) -> &'static str {
        match self {
            Accel::Auto => "Automatic",
            // Named for what this host actually has — the same setting,
            // spelled the way the machine in front of the user spells it
            // (`crate::player::hw_accel_label`).
            Accel::Kvm if cfg!(target_os = "windows") => "WHPX (required)",
            Accel::Kvm if cfg!(target_os = "linux") => "KVM (required)",
            Accel::Kvm => "Hardware acceleration (required)",
            Accel::Tcg => "Emulation",
        }
    }
}

/// One of the QEMU fast paths this project carries as its own patches
/// (`patches/qemu/README.md`, docs 13 and 16, the M8/M9 tracks), with
/// the off switch the patch already gave it.
///
/// Every one of them is *the emulator running the guest's arithmetic on
/// the host's own silicon instead of simulating it*, which is why each
/// arrived with an off switch: the switch is the oracle. When a guest
/// computes the wrong number or a game stops drawing, one run with one
/// of these off says whether a fast path did it — the alternative is
/// bisecting a patch queue against a Windows install.
///
/// They therefore only exist under emulation. A machine running on KVM
/// executes on the host CPU directly and none of these is reachable;
/// the form says so rather than showing seven switches that do nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Optimization {
    X87Fast,
    SseFast,
    SimdFast,
    RepFast,
    SmcSameValue,
    InlineLookup,
    PinnedRegs,
}

/// Where an optimization's switch goes on the command line: a property
/// of the guest CPU (`-cpu pentium3,x87-fast=off`) or of the TCG
/// accelerator itself (`-accel tcg,smc-same-value=off`). The two are not
/// interchangeable — QEMU looks each name up on a different object — and
/// the accelerator half is the reason `qemu_args` spells the accelerator
/// as `-accel` rather than `-machine accel=`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Knob {
    Cpu,
    Tcg,
}

impl Optimization {
    /// In the order the form lists them: the arithmetic fast paths
    /// first, in the order they were written, then the two that are
    /// about translation, then the experimental one.
    pub const ALL: [Optimization; 7] = [
        Optimization::X87Fast,
        Optimization::SseFast,
        Optimization::SimdFast,
        Optimization::RepFast,
        Optimization::SmcSameValue,
        Optimization::InlineLookup,
        Optimization::PinnedRegs,
    ];

    /// The QEMU property name, which is also the key in `machine.toml`:
    /// one name for the thing, so a bundle can be read against
    /// `patches/qemu/README.md` without a translation table.
    pub fn key(self) -> &'static str {
        match self {
            Optimization::X87Fast => "x87-fast",
            Optimization::SseFast => "sse-fast",
            Optimization::SimdFast => "simd-fast",
            Optimization::RepFast => "rep-fast",
            Optimization::SmcSameValue => "smc-same-value",
            Optimization::InlineLookup => "inline-lookup",
            Optimization::PinnedRegs => "pinned-regs",
        }
    }

    fn knob(self) -> Knob {
        match self {
            Optimization::X87Fast
            | Optimization::SseFast
            | Optimization::SimdFast
            | Optimization::RepFast => Knob::Cpu,
            Optimization::SmcSameValue | Optimization::InlineLookup | Optimization::PinnedRegs => Knob::Tcg,
        }
    }

    /// Whether a machine that says nothing has it on. Everything that
    /// has shipped is on — turning one off is a diagnosis, not a
    /// preference — and `pinned-regs` is off because the patch itself is
    /// off by default while the work is in progress.
    pub fn default_on(self) -> bool {
        self != Optimization::PinnedRegs
    }

    /// The checkbox's label: what the fast path does, not what it is
    /// called in the patch queue.
    pub fn label(self) -> &'static str {
        match self {
            Optimization::X87Fast => "x87 floating point on the host FPU",
            Optimization::SseFast => "SSE floating point on the host",
            Optimization::SimdFast => "MMX and SSE integer instructions inline",
            Optimization::RepFast => "Block string moves as whole-page copies",
            Optimization::SmcSameValue => "Skip retranslation when code is rewritten unchanged",
            Optimization::InlineLookup => "Find the next block without leaving generated code",
            Optimization::PinnedRegs => "Keep guest registers in host registers (experimental)",
        }
    }

    /// The sentence under it: what it buys, and — for the one case where
    /// it matters — why it is off.
    pub fn note(self) -> &'static str {
        match self {
            Optimization::X87Fast => {
                "Windows and Direct3D run the x87 unit at a precision the host reproduces exactly, \
                 so the guest's floating point is executed rather than simulated. Super PI 1M on the \
                 M1 Air: 9:49 off, 1:57 on. Turn it off if a guest's arithmetic looks wrong."
            }
            Optimization::SseFast => {
                "Packed and scalar SSE/SSE2 arithmetic on the host's vector unit instead of a call \
                 per instruction: 7-12x on packed code, 3-4x on scalar."
            }
            Optimization::SimdFast => {
                "MMX and SSE integer maths, shuffles and packs become host vector instructions: \
                 2-4x on the pixel loops of an era software renderer."
            }
            Optimization::RepFast => {
                "REP MOVS / STOS copies a page at a time through memcpy instead of one element per \
                 loop - 30x on the blits an era game fills the screen with."
            }
            Optimization::SmcSameValue => {
                "Self-modifying code usually writes back the bytes already there, and rewriting a \
                 value with itself cannot invalidate anything. Moto Racer's race: 7.3 to 21.7 fps."
            }
            Optimization::InlineLookup => {
                "Every return and indirect jump finds its next block in generated code rather than \
                 through a helper call: 7-Zip in the guest, +7-12%."
            }
            Optimization::PinnedRegs => {
                "Apple Silicon only, and still being worked on - a boot crash has been seen with it \
                 on. Leave it off unless you are testing it."
            }
        }
    }
}

/// A machine's optimization settings: **only what differs from
/// `Optimization::default_on` is stored**, so a bundle that says nothing
/// runs exactly the way every bundle written before this field existed
/// did, and an optimization added to the patch queue later arrives on in
/// every bundle that already exists.
///
/// Keyed by the QEMU property name rather than by the enum, so a bundle
/// written by a launcher that knows an optimization this build does not
/// survives a load and a save with the entry intact instead of being
/// refused as an unknown variant.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct Optimizations(BTreeMap<String, bool>);

impl Optimizations {
    pub fn enabled(&self, opt: Optimization) -> bool {
        self.0.get(opt.key()).copied().unwrap_or_else(|| opt.default_on())
    }

    pub fn is_default(&self, opt: Optimization) -> bool {
        self.enabled(opt) == opt.default_on()
    }

    /// Turn one on or off. Putting it back on its own default *removes*
    /// the entry rather than writing it out, which is what keeps a
    /// `machine.toml` holding only what someone actually changed.
    pub fn set(&mut self, opt: Optimization, on: bool) {
        if on == opt.default_on() {
            self.0.remove(opt.key());
        } else {
            self.0.insert(opt.key().to_string(), on);
        }
    }

    /// Every optimization back on its default. Only the ones this build
    /// knows about: an entry a newer launcher wrote is not something
    /// this one can decide is wrong.
    pub fn reset(&mut self) {
        for opt in Optimization::ALL {
            self.0.remove(opt.key());
        }
    }

    pub fn all_default(&self) -> bool {
        Optimization::ALL.iter().all(|opt| self.is_default(*opt))
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// What a collapsed section says about itself, so someone who never
    /// opens it still sees that something in there has been changed.
    /// A count rather than "all on", because one of them ships off.
    pub fn summary(&self) -> String {
        let on = Optimization::ALL.iter().filter(|opt| self.enabled(**opt)).count();
        format!("{on} of {} on", Optimization::ALL.len())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Machine {
    pub name: String,
    pub family: Family,
    pub ram_mb: u32,
    /// How to execute the guest, or `None` for "whatever this family
    /// runs as" (`default_accel`). Absent rather than defaulted, so a
    /// bundle written before this field existed follows its family
    /// instead of silently acquiring KVM — which for a Win98 machine
    /// would be a *change* to how it had been running. Anything this
    /// launcher saves carries an explicit value: the form always has one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub accel: Option<Accel>,
    /// Whether the machine has a network adapter at all (doc 06's
    /// per-family NIC on QEMU's user-mode NAT). `false` gives the guest
    /// no adapter rather than an unplugged one: "no networking" should
    /// mean Windows never sees a card, never asks for its driver and
    /// never waits on a network at boot.
    ///
    /// Defaults to `true` when the field is absent, which is how every
    /// bundle written before it existed ran — a machine must not lose
    /// its network by being read by a newer launcher.
    #[serde(default = "network_enabled_default")]
    pub network: bool,
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

    /// A floppy image in the machine's A: drive, if it has one. Doc 06
    /// lists a floppy on the Win98 machine ("driver/utility sneakernet,
    /// boot disks") and doc 07 lists floppy images among the media the
    /// launcher handles; a DOS machine may boot from one. Absent means
    /// no disk in the drive — the controller is there either way, as on
    /// a real PC of the era.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub floppy: Option<PathBuf>,

    /// Which drive to boot from. Absent = `Boot::Auto`, which is what
    /// every bundle written before this field existed was doing.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub boot: Option<Boot>,

    /// How fast the CPU is allowed to run (`CpuSpeed`). Absent =
    /// unthrottled, again what every earlier bundle was doing.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cpu_speed: Option<CpuSpeed>,

    /// Which of our own emulator fast paths this machine runs with
    /// (`Optimization`), holding only what someone turned off — absent
    /// means all of them at their shipped setting.
    ///
    /// **Last in the struct on purpose.** It is the only field that
    /// serializes to a TOML *table*, and a table swallows every
    /// key-value line that follows it: written anywhere else, the fields
    /// after it would be read back as part of `[optimizations]`.
    #[serde(default, skip_serializing_if = "Optimizations::is_empty")]
    pub optimizations: Optimizations,
}

/// How a family runs unless the machine says otherwise.
///
/// **Win98 is emulated by default.** KVM runs the guest at host speed,
/// and doc 06's `pentium3` model does not protect against Win9x's
/// fast-CPU bugs — it is the *speed* that trips them, not the CPUID. TCG
/// is also the path this project's own x87/SSE fast paths (docs 13, 16)
/// exist for, so it is the configuration Win98 is actually tuned and
/// tested on here. XP has none of those problems and wants the speed.
pub fn default_accel(family: Family) -> Accel {
    match family {
        // A DOS machine is throttled by default and a throttle needs TCG
        // (`-icount` and KVM cannot coexist), so this is the only honest
        // default; `Auto` would promise KVM and not deliver it.
        Family::Win98 | Family::Dos => Accel::Tcg,
        Family::Xp => Accel::Auto,
    }
}

/// Networking is on unless a bundle says otherwise: an era machine came
/// with a network card, and this is what every machine did before the
/// field existed.
fn network_enabled_default() -> bool {
    true
}

/// Whether a *new* machine of this family gets a card. DOS is the one
/// that doesn't: it reaches a network only through a packet driver the
/// user installs by hand, so the card would be an unused device the
/// guest still enumerates. (An existing bundle with no `network` field
/// is unaffected — that is `network_enabled_default`, and it stays on
/// for every family, because turning a card off under a machine that has
/// been running with one is a hardware change, not a default.)
pub fn default_network(family: Family) -> bool {
    family != Family::Dos && network_enabled_default()
}

/// The speed a family runs at unless the machine says otherwise. Only
/// DOS is throttled: a 486DX2-66 is the machine most of the CD-ROM era
/// was written for, and it is inside the range where the cap is exact
/// (see `CpuSpeed`). Windows machines are unthrottled — 9x and XP read
/// the clock instead of counting instructions, and a throttle would only
/// make them slow.
pub fn default_cpu_speed(family: Family) -> CpuSpeed {
    match family {
        Family::Dos => CpuSpeed::Dx266,
        Family::Win98 | Family::Xp => CpuSpeed::Unthrottled,
    }
}

/// doc 06's RAM default for a family.
pub fn default_ram_mb(family: Family) -> u32 {
    match family {
        Family::Win98 => 256, // doc 06: 256 MB default, ≤512 MB hard cap
        Family::Xp => 512,    // doc 06: 512 MB-1 GB default
        // DOS itself uses the first megabyte; the rest is XMS for the
        // extenders a mid-90s game ships with, and more of it buys
        // nothing. 64 MB is generous for the era and stays inside what
        // MS-DOS 6.22's own HIMEM.SYS manages.
        Family::Dos => 64,
    }
}

/// A value inside a QEMU option string. Options are separated by commas
/// there, so a comma in a value is written twice — otherwise a disk in
/// `~/Games/Doom, Quake and friends/` silently becomes an unknown option
/// and QEMU refuses the whole line. Paths come from a file picker, and a
/// comma in a directory name is entirely ordinary.
fn opt_value(s: &str) -> String {
    s.replace(',', ",,")
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
        Family::Dos => 4..=256,
    }
}

impl Machine {
    /// A new machine from doc 06's reference defaults for `family`.
    pub fn reference(family: Family, name: String, disk: PathBuf) -> Self {
        Machine {
            name,
            family,
            ram_mb: default_ram_mb(family),
            accel: Some(default_accel(family)),
            network: default_network(family),
            disk,
            disc: None,
            discs: Vec::new(),
            shader_profile: None,
            shader: None,
            floppy: None,
            boot: None,
            cpu_speed: Some(default_cpu_speed(family)),
            optimizations: Optimizations::default(),
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
    /// The accelerator, as `-accel` options. `Auto` is expressed as
    /// QEMU's own fallback list rather than by probing `/dev/kvm` here:
    /// the answer a probe gives can still be wrong at spawn time
    /// (permissions, a module unloaded since), and QEMU's list already
    /// means exactly "KVM if you can, emulation otherwise" — two
    /// `-accel` options are tried in order and the first that
    /// initializes wins, which is the same code path `accel=kvm:tcg`
    /// took. `kvm` is only offered where it exists at all — on macOS the
    /// name is not a registered accelerator, and listing it there would
    /// print a warning on every boot for nothing.
    ///
    /// **`-accel`, not `-machine accel=`, and the two cannot be mixed**
    /// ("The -accel and \"-machine accel=\" options are incompatible",
    /// `system/vl.c`). The TCG-side optimizations are properties of the
    /// accelerator object, and `-accel` is the only spelling that has
    /// somewhere to put them.
    fn accel_args(&self) -> Vec<String> {
        let mut tcg = "tcg".to_string();
        tcg.push_str(&self.optimization_props(Knob::Tcg));
        match self.effective_accel() {
            // "hardware acceleration, required": spelled the way the
            // host in hand spells it (`whpx` on Windows), so a machine
            // directory carries between them meaning the same thing.
            Accel::Kvm if cfg!(target_os = "windows") => vec!["-accel".into(), "whpx".into()],
            Accel::Kvm => vec!["-accel".into(), "kvm".into()],
            Accel::Auto if cfg!(target_os = "linux") => {
                vec!["-accel".into(), "kvm".into(), "-accel".into(), tcg]
            }
            Accel::Auto if cfg!(target_os = "windows") => {
                vec!["-accel".into(), "whpx".into(), "-accel".into(), tcg]
            }
            Accel::Auto | Accel::Tcg => vec!["-accel".into(), tcg],
        }
    }

    /// The `,name=on`/`,name=off` tail for one kind of switch — **only
    /// what differs from the property's own default in our QEMU**, so a
    /// machine that has changed nothing produces the command line it
    /// always produced, and one run against a QEMU without these patches
    /// still starts.
    fn optimization_props(&self, knob: Knob) -> String {
        let mut out = String::new();
        for opt in Optimization::ALL {
            if opt.knob() == knob && !self.optimizations.is_default(opt) {
                let value = if self.optimizations.enabled(opt) { "on" } else { "off" };
                out.push_str(&format!(",{}={value}", opt.key()));
            }
        }
        out
    }

    /// What this machine actually runs as: its own setting, or its
    /// family's (`default_accel`) when the bundle doesn't say —
    /// **except** that a throttled CPU forces emulation, because QEMU
    /// refuses `-icount` together with KVM ("cannot enable icount when
    /// KVM is enabled") and starting is better than being right about
    /// the accelerator. The wizard says so next to the field, so this
    /// never happens behind someone's back.
    pub fn effective_accel(&self) -> Accel {
        if self.effective_cpu_speed().icount_shift().is_some() {
            return Accel::Tcg;
        }
        self.accel.unwrap_or_else(|| default_accel(self.family))
    }

    /// The CPU speed this machine runs at: its own setting, or its
    /// family's default (only DOS has a throttled one).
    pub fn effective_cpu_speed(&self) -> CpuSpeed {
        self.cpu_speed.unwrap_or_else(|| default_cpu_speed(self.family))
    }

    pub fn effective_boot(&self) -> Boot {
        self.boot.unwrap_or_default()
    }

    pub fn qemu_args(&self, pc_bios_dir: &Path, shelf: Option<&Path>) -> Vec<String> {
        let mut args = vec!["-L".into(), pc_bios_dir.display().to_string(), "-machine".into(), "pc".into()];
        args.extend(self.accel_args());
        args.extend([
            "-m".into(),
            self.ram_mb.to_string(),
            // doc 06's floor for both families: avoids the fast-CPU Win9x
            // bugs and CPUID-dispatched guest code that mis-decodes under
            // -cpu host (the Max Payne JPEG decoder gotcha)
            "-cpu".into(),
            format!("pentium3{}", self.optimization_props(Knob::Cpu)),
            "-drive".into(),
            format!("file={},if=ide,index=0,media=disk", opt_value(&self.disk.display().to_string())),
            "-usb".into(),
            "-device".into(),
            "usb-tablet".into(),
        ]);
        // The CPU rate, when the machine asks for one. `align=on` is the
        // whole point and not a detail: `-icount shift=N` on its own only
        // makes the *guest's* clock a function of instructions retired,
        // which leaves the guest believing it is slow while the host runs
        // it as fast as it likes — measured 2026-09-06, a run that was
        // meant to be throttled finished in less wall-clock time than the
        // unthrottled one, because the guest's idle waits collapse too.
        if let Some(shift) = self.effective_cpu_speed().icount_shift() {
            args.extend(["-icount".into(), format!("shift={shift},align=on")]);
        }
        // The floppy: `format=raw` because a floppy image has no header
        // to probe and QEMU otherwise both warns and refuses writes to
        // the boot sector.
        if let Some(floppy) = &self.floppy {
            args.extend(["-drive".into(), format!("file={},if=floppy,index=0,format=raw", opt_value(&floppy.display().to_string()))]);
        }
        if let Some(order) = self.effective_boot().order() {
            args.extend(["-boot".into(), format!("order={order}")]);
        }
        // Doc 06's per-family NIC on QEMU's user-mode NAT, or no adapter
        // at all — not an unplugged cable: a card that is present would
        // still make Windows enumerate it, ask for its driver on a fresh
        // install and wait on it at boot, none of which is what turning
        // networking off is for.
        //
        // `-nic none` is the half that actually turns it off. QEMU
        // *creates a NIC of its own* when the command line asks for no
        // networking at all — leaving out the `-netdev` doesn't remove
        // the card, it only replaces ours with an e1000 in the slot
        // below (`query-pci` says so), which is the opposite of what the
        // setting means.
        //
        // The XP devices carry explicit PCI addresses because removing
        // the NIC would otherwise slide the sound card up into its slot,
        // and a card that moves is a hardware change an installed
        // Windows re-detects. These are the addresses those devices
        // already get from their `-device` order today, so pinning them
        // changes nothing for an existing machine — it only keeps them
        // still when the NIC comes and goes. Win98 needs none of this:
        // its display is `-vga` (not a `-device`) and its SB16 is ISA,
        // so its NIC is the only card in the sequence.
        if !self.network {
            args.extend(["-nic".into(), "none".into()]);
        }
        match self.family {
            Family::Win98 => {
                args.extend(["-vga".into(), "cirrus".into()]);
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    args.extend(["-device".into(), "pcnet,netdev=n0".into()]); // in-box 98 driver
                }
                args.extend(["-device".into(), "sb16,audiodev=embed0".into()]);
            }
            // The 1994 PC: the same chipset and the SB16 doc 06 already
            // puts on the Win98 machine "for DOS boxes/games", the cirrus
            // adapter for its VGA and VESA modes, and nothing else. No
            // 3D of any kind is reachable from DOS here — the Glide
            // wrapper for DOS is GLIDE2X.OVL, which we do not build.
            Family::Dos => {
                args.extend(["-vga".into(), "cirrus".into()]);
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    args.extend(["-device".into(), "pcnet,netdev=n0".into()]);
                }
                args.extend(["-device".into(), "sb16,audiodev=embed0".into()]);
            }
            Family::Xp => {
                args.extend(["-vga".into(), "none".into()]);
                args.extend(["-device".into(), "d3dpt-vga,addr=0x02".into()]);
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    // in-box XP driver
                    args.extend(["-device".into(), "rtl8139,netdev=n0,addr=0x03".into()]);
                }
                args.extend(["-device".into(), "AC97,audiodev=embed0,addr=0x04".into()]);
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
            // `qemu_medium`, not the path: a shared folder is a disc too,
            // and it is named to QEMU with the `isodir:` prefix (M5g).
            drive.push_str(&format!(",file={}", opt_value(&crate::disc_library::qemu_medium(disc))));
        }
        let mut cd = format!("ide-cd,bus=ide.1,id={},drive=cd0,audiodev=embed0", crate::control::CDROM_ID);
        if let Some(shelf) = shelf {
            // The drive answers the in-guest CDSHELF program from this
            // file (patch 52). Without it the vendor command reports
            // "no shelf" and the drive is an ordinary CD-ROM — which is
            // also what a hand-written bundle run straight through
            // `player` gets.
            cd.push_str(&format!(",shelf={}", opt_value(&shelf.display().to_string())));
        }
        args.extend(["-drive".into(), drive, "-device".into(), cd]);
        args
    }
}
