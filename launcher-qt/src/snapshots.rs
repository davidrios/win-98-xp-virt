//! The toolkit-free half of `launcher/src/snapshots.rs`, copied.
//!
//! This is the one place the port could not just `#[path]`-include a
//! shared module, and it is worth being explicit about why: that file
//! holds two unrelated things in one — a `Snapshot` type with the
//! `qemu-img` / QMP calls around it (below, pure), and `SnapshotWindow`,
//! a struct that mixes the *state machine* (which source the list comes
//! from, the in-flight job, the pending restore confirmation) with the
//! egui that draws it. The state machine is toolkit-independent and this
//! port wants it; `egui::Context` in the same file is what stops it
//! being shared.
//!
//! So the free functions below are a verbatim copy, and the state
//! machine is re-expressed as a QObject in `src/qt/snaps.rs`. Splitting
//! the upstream file into `snapshots.rs` + `snapshots_ui.rs` would
//! remove this copy — see the findings in `docs/07-launcher.md`.
//! `control.rs`, shared verbatim, refers to `crate::snapshots::Snapshot`,
//! so the type has to keep this module name.


use crate::player;
use std::path::Path;
use std::process::Command;

#[derive(Debug, Clone)]
pub struct Snapshot {
    pub id: String,
    pub name: String,
    /// Size of the saved CPU/RAM state, 0 for a disk-only snapshot
    /// (`qemu-img snapshot -c` makes those; `savevm` makes the other kind).
    pub vm_state_size: u64,
    /// Unix seconds when it was taken, as qcow2 records it.
    pub date_sec: u64,
}

impl Snapshot {
    /// `2026-09-05 14:03` in local time, or the raw seconds if that can't
    /// be formed — this is a label in a list, never a parsed value.
    pub fn date_label(&self) -> String {
        let secs = self.date_sec as i64;
        // No chrono/time dependency for one label: civil-from-days
        // (Howard Hinnant's algorithm), UTC. A snapshot list sorted by a
        // timestamp that's an hour off in the user's head is not worth a
        // timezone database.
        let days = secs.div_euclid(86_400);
        let rem = secs.rem_euclid(86_400);
        let z = days + 719_468;
        let era = z.div_euclid(146_097);
        let doe = z.rem_euclid(146_097);
        let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
        let y = yoe + era * 400;
        let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        let mp = (5 * doy + 2) / 153;
        let d = doy - (153 * mp + 2) / 5 + 1;
        let m = if mp < 10 { mp + 3 } else { mp - 9 };
        let y = if m <= 2 { y + 1 } else { y };
        format!("{y:04}-{m:02}-{d:02} {:02}:{:02} UTC", rem / 3600, (rem % 3600) / 60)
    }

    /// "12.4 MB" / "—" for a disk-only snapshot.
    pub fn size_label(&self) -> String {
        if self.vm_state_size == 0 {
            return "—".into();
        }
        let mb = self.vm_state_size as f64 / (1024.0 * 1024.0);
        if mb >= 1024.0 {
            format!("{:.1} GB", mb / 1024.0)
        } else {
            format!("{mb:.1} MB")
        }
    }
}

fn qemu_img(args: &[&str], disk: &Path) -> std::io::Result<std::process::Output> {
    let bin = player::qemu_img_binary();
    Command::new(&bin)
        .args(args)
        .arg(disk)
        .output()
        .map_err(|e| std::io::Error::other(format!("running {}: {e}", bin.display())))
}

/// Fail with `qemu-img`'s own stderr rather than a bare exit code — its
/// messages ("Could not find snapshot 'x'", "Permission denied") are
/// exactly what the window should show.
fn check(what: &str, out: &std::process::Output) -> std::io::Result<()> {
    if out.status.success() {
        return Ok(());
    }
    let err = String::from_utf8_lossy(&out.stderr);
    let err = err.trim();
    Err(std::io::Error::other(if err.is_empty() {
        format!("qemu-img {what}: exited with {}", out.status)
    } else {
        format!("qemu-img {what}: {err}")
    }))
}

/// Every internal snapshot in `disk`, newest last (qcow2 order). An image
/// with no snapshot table has none — not an error.
pub fn list(disk: &Path) -> std::io::Result<Vec<Snapshot>> {
    let out = qemu_img(&["info", "--output=json"], disk)?;
    check("info", &out)?;
    let info: serde_json::Value = serde_json::from_slice(&out.stdout).map_err(std::io::Error::other)?;
    Ok(parse(info.get("snapshots")))
}

/// The `snapshots` array of an `ImageInfo`, whichever way it arrived:
/// `qemu-img info --output=json` (offline) and QMP
/// `query-named-block-nodes` (live) return the same shape, so the window
/// shows one kind of row either way.
pub fn parse(list: Option<&serde_json::Value>) -> Vec<Snapshot> {
    let Some(list) = list.and_then(|s| s.as_array()) else {
        return Vec::new();
    };
    list.iter()
        .map(|s| Snapshot {
            id: s["id"].as_str().unwrap_or_default().to_string(),
            name: s["name"].as_str().unwrap_or_default().to_string(),
            vm_state_size: s["vm-state-size"].as_u64().unwrap_or(0),
            date_sec: s["date-sec"].as_u64().unwrap_or(0),
        })
        .collect()
}

pub fn create(disk: &Path, name: &str) -> std::io::Result<()> {
    check("snapshot -c", &qemu_img(&["snapshot", "-c", name], disk)?)
}

pub fn delete(disk: &Path, name: &str) -> std::io::Result<()> {
    check("snapshot -d", &qemu_img(&["snapshot", "-d", name], disk)?)
}

pub fn restore(disk: &Path, name: &str) -> std::io::Result<()> {
    check("snapshot -a", &qemu_img(&["snapshot", "-a", name], disk)?)
}
