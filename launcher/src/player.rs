//! Spawning `player` (doc 07: two separate binaries). Once spawned the
//! process is independent of the launcher: dropping the `Child` neither
//! waits nor kills it (Rust's default), which is what we want — closing
//! the launcher, or the grid forgetting about a bundle, must never stop
//! a running guest (CLAUDE.md: a killed VM leaves a dirty FAT; only a
//! guest-side shutdown, or the player's own window, should end a run).

use crate::bundle::Machine;
use crate::shader_library;
use crate::shader_profile::ShaderProfile;
use std::path::PathBuf;
use std::process::{Child, Command};

/// The `player` binary: `bin/2ksbox-player` in an installed tree
/// (`paths.rs`), otherwise alongside the launcher's own executable, where
/// both sit in dev (workspace binaries in the same `target/<profile>`
/// directory). `LAUNCHER_PLAYER_BIN` overrides both.
pub fn player_binary() -> PathBuf {
    if let Ok(p) = std::env::var("LAUNCHER_PLAYER_BIN") {
        return p.into();
    }
    if let Some(prefix) = crate::paths::install_prefix() {
        let name = if cfg!(windows) { "2ksbox-player.exe" } else { "2ksbox-player" };
        return prefix.join("bin").join(name);
    }
    let exe = std::env::current_exe().expect("current_exe");
    let dir = exe.parent().expect("executable has a parent directory");
    dir.join(if cfg!(windows) { "player.exe" } else { "player" })
}

/// Whether this host can actually give a guest KVM, for the wizard to
/// say so next to the acceleration picker. Deliberately not consulted by
/// `bundle::qemu_args`, which leaves the decision to QEMU's own
/// `accel=kvm:tcg` fallback: this is a hint for a human, and being open
/// to *write* is the part a bare `exists()` would miss (the device node
/// is there on a host whose user is not in the `kvm` group, and that is
/// the common way for this to be unavailable).
pub fn kvm_available() -> bool {
    cfg!(target_os = "linux")
        && std::fs::OpenOptions::new().read(true).write(true).open("/dev/kvm").is_ok()
}

/// QEMU's firmware directory (README's `-L`): shipped as
/// `share/2ksbox/pc-bios` in an installed tree, `qemu/pc-bios` in
/// a checkout (`paths.rs`). `LAUNCHER_PC_BIOS_DIR` overrides both.
pub fn pc_bios_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("LAUNCHER_PC_BIOS_DIR") {
        return dir.into();
    }
    crate::paths::resource("share/2ksbox/pc-bios", "qemu/pc-bios")
}

/// Resolve `machine`'s shader setting into the preset+overrides the
/// player actually runs with: a named `shader_profile` (looked up in the
/// profile library) takes precedence, then the raw `shader` override,
/// then no shader at all. A `shader_profile` naming a deleted profile
/// silently falls through to `shader`/none rather than failing the
/// machine (see `shader_library::find`).
fn resolve_shader(machine: &Machine) -> Option<ShaderProfile> {
    if let Some(id) = &machine.shader_profile {
        if let Some(profile) = shader_library::find(&shader_library::default_dir(), id) {
            return Some(profile);
        }
    }
    machine.shader.clone().map(|preset| ShaderProfile::new(String::new(), preset))
}

/// The `--shader [path] [--shader-params k=v,...]` arguments `spawn`
/// passes to `player`, given `machine`'s resolved shader setting. Split
/// out so `main.rs`'s `--print-shader-args` debug verb can show exactly
/// what a bundle resolves to without spawning anything.
pub fn shader_args(machine: &Machine) -> Vec<String> {
    let Some(profile) = resolve_shader(machine) else {
        return Vec::new();
    };
    let mut args = vec!["--shader".to_string(), profile.preset.display().to_string()];
    if let Some(params) = profile.params_arg() {
        args.push("--shader-params".to_string());
        args.push(params);
    }
    args
}

/// Spawn `player` on `machine`. Inherits the launcher's stdout/stderr for
/// now (dev convenience); a real log file is a packaging-time concern.
///
/// `qmp_socket`, when given, makes QEMU listen on that path for a second
/// monitor the launcher drives for live media/snapshot control
/// (`control.rs`) — the player's own in-process monitor is untouched and
/// the player itself needs no change, since everything after `--` is
/// passed through to QEMU. `shelf` is the flat disc-shelf file the
/// drive answers the in-guest CDSHELF program from.
pub fn spawn(
    machine: &Machine,
    qmp_socket: Option<&std::path::Path>,
    shelf: Option<&std::path::Path>,
) -> std::io::Result<Child> {
    let mut args = machine.qemu_args(&pc_bios_dir(), shelf);
    if let Some(extra) = qmp_socket.and_then(crate::control::qmp_args) {
        args.extend(extra);
    }
    let bin = player_binary();
    Command::new(&bin)
        .args(shader_args(machine))
        .arg("--")
        .args(args)
        .spawn()
        .map_err(|e| std::io::Error::other(format!("running {}: {e}", bin.display())))
}

/// `qemu-img`, a QEMU build product rather than a workspace binary, so it
/// doesn't sit next to the launcher/player like `player_binary` does. In
/// an installed tree it is `libexec/2ksbox/qemu-img` — deliberately
/// not `bin/`, since it is *our* patched build and must not shadow (or be
/// shadowed by) the system's own on `PATH`; in a checkout it is
/// `build/qemu/qemu-img`, the same path the test scripts use.
/// `LAUNCHER_QEMU_IMG_BIN` overrides both.
pub fn qemu_img_binary() -> PathBuf {
    if let Ok(bin) = std::env::var("LAUNCHER_QEMU_IMG_BIN") {
        return bin.into();
    }
    crate::paths::resource("libexec/2ksbox/qemu-img", "build/qemu/qemu-img")
}

/// Create a new qcow2 disk image for the wizard's "new disk" path.
pub fn create_disk(path: &std::path::Path, size_gb: u32) -> std::io::Result<()> {
    let bin = qemu_img_binary();
    let status = Command::new(&bin)
        .args(["create", "-f", "qcow2"])
        .arg(path)
        .arg(format!("{size_gb}G"))
        .status()
        .map_err(|e| std::io::Error::other(format!("running {}: {e}", bin.display())))?;
    if status.success() {
        Ok(())
    } else {
        Err(std::io::Error::other(format!("qemu-img create exited with {status}")))
    }
}
