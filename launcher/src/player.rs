//! Spawning `player` (doc 07: two separate binaries). Once spawned the
//! process is independent of the launcher: dropping the `Child` neither
//! waits nor kills it (Rust's default), which is what we want — closing
//! the launcher, or the grid forgetting about a bundle, must never stop
//! a running guest (CLAUDE.md: a killed VM leaves a dirty FAT; only a
//! guest-side shutdown, or the player's own window, should end a run).

use crate::bundle::Machine;
use std::path::PathBuf;
use std::process::{Child, Command};

/// The `player` binary. In dev it sits alongside the launcher's own
/// executable (both are workspace binaries in the same `target/<profile>`
/// directory); `LAUNCHER_PLAYER_BIN` overrides it.
pub fn player_binary() -> PathBuf {
    if let Ok(p) = std::env::var("LAUNCHER_PLAYER_BIN") {
        return p.into();
    }
    let exe = std::env::current_exe().expect("current_exe");
    let dir = exe.parent().expect("executable has a parent directory");
    dir.join(if cfg!(windows) { "player.exe" } else { "player" })
}

/// `qemu/pc-bios` (README's `-L`). Bundling this is a packaging (M6 step
/// 6) concern; for now it's found relative to a workspace checkout.
/// `LAUNCHER_PC_BIOS_DIR` overrides it.
pub fn pc_bios_dir() -> PathBuf {
    std::env::var("LAUNCHER_PC_BIOS_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("qemu/pc-bios"))
}

/// Spawn `player` on `machine`. Inherits the launcher's stdout/stderr for
/// now (dev convenience); a real log file is a packaging-time concern.
pub fn spawn(machine: &Machine) -> std::io::Result<Child> {
    let args = machine.qemu_args(&pc_bios_dir());
    Command::new(player_binary()).arg("--").args(args).spawn()
}

/// `qemu-img`, a QEMU build product rather than a workspace binary, so it
/// doesn't sit next to the launcher/player like `player_binary` does;
/// found the same way test scripts already do (`build/qemu/qemu-img`
/// relative to a workspace checkout). `LAUNCHER_QEMU_IMG_BIN` overrides
/// it. Bundling a copy is a packaging (M6 step 6) concern.
pub fn qemu_img_binary() -> PathBuf {
    std::env::var("LAUNCHER_QEMU_IMG_BIN")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("build/qemu/qemu-img"))
}

/// Create a new qcow2 disk image for the wizard's "new disk" path.
pub fn create_disk(path: &std::path::Path, size_gb: u32) -> std::io::Result<()> {
    let status = Command::new(qemu_img_binary())
        .args(["create", "-f", "qcow2"])
        .arg(path)
        .arg(format!("{size_gb}G"))
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(std::io::Error::other(format!("qemu-img create exited with {status}")))
    }
}
