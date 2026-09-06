//! Live control of a running machine: media swap and snapshots on a
//! guest that's already up (doc 07's "runtime disc mount/eject/swap from
//! the disc shelf" and "QEMU internal snapshots via in-proc QMP,
//! surfaced in the overlay *and the launcher*").
//!
//! **No new protocol and no player change.** The launcher adds
//! `-qmp unix:<path>,server,nowait` to the arguments it spawns the
//! player with and talks QMP to that socket itself — exactly what
//! `tools/qmpc.py` already does to drive a guest. QEMU allows several
//! monitors, so the player's own in-process one (`player/src/qmp.rs`, on
//! a socketpair with no filesystem path at all) is untouched and neither
//! binary grows an IPC surface of its own. A hand-written bundle run
//! straight through `player` simply has no launcher socket, which is the
//! documented "the launcher is optional" path (doc 07).
//!
//! Unix sockets only, so this is Linux/macOS. On Windows the socket is
//! never created and every live operation reports that; a named pipe or
//! a loopback port is a packaging-time (M6 step 6) question.

use serde_json::Value;
use std::path::{Path, PathBuf};

/// Where a machine's monitor socket lives: the platform runtime dir
/// (`/run/user/<uid>/2ksbox` on Linux) or the temp dir, plus a
/// name derived from the bundle directory. Unix socket paths are capped
/// around 108 bytes, so the name is the bundle's own directory name cut
/// short plus a hash of its full path — short, readable, and still
/// unique across two libraries holding a same-named bundle.
pub fn socket_path(bundle_dir: &Path) -> PathBuf {
    let dir = crate::paths::runtime_dir();
    // FNV-1a over the full path: not security, just collision avoidance.
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for b in bundle_dir.as_os_str().as_encoded_bytes() {
        hash ^= *b as u64;
        hash = hash.wrapping_mul(0x1000_0000_01b3);
    }
    let stem: String =
        bundle_dir.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default().chars().take(24).collect();
    dir.join(format!("{stem}-{hash:016x}.qmp"))
}

/// The `-qmp` argument that makes QEMU listen on `path`, or `None` on a
/// platform without Unix sockets. Also removes a stale socket file left
/// by a player that was killed rather than shut down — QEMU refuses to
/// bind over one.
/// The flat shelf file this machine's ATAPI drive reads
/// (`cdshelf/cdshelf_proto.h`), beside its monitor socket: both are
/// per-run, per-machine host state that belongs in the runtime dir
/// rather than in the bundle.
pub fn shelf_path(bundle_dir: &Path) -> PathBuf {
    socket_path(bundle_dir).with_extension("shelf")
}

/// A QMP monitor is complete control of the machine, so the directory
/// holding these sockets is owner-only. The platform runtime dir already
/// is (`/run/user/<uid>` is 0700, macOS's per-user `$TMPDIR` likewise),
/// but our own subdirectory under it is created here, so it says so
/// rather than inheriting whatever the umask happens to be.
pub fn qmp_args(path: &Path) -> Option<Vec<String>> {
    if !cfg!(unix) {
        return None;
    }
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(parent, std::fs::Permissions::from_mode(0o700));
        }
    }
    // A player that was killed rather than shut down leaves the socket
    // file behind, and QEMU refuses to bind over one.
    let _ = std::fs::remove_file(path);
    Some(vec!["-qmp".into(), format!("unix:{},server,nowait", path.display())])
}

#[cfg(unix)]
mod imp {
    use serde_json::{json, Value};
    use std::io::{BufRead, BufReader, Write};
    use std::os::unix::net::UnixStream;
    use std::path::Path;
    use std::time::Duration;

    /// A connected QMP session. Synchronous and single-threaded: the
    /// launcher is the only client of this socket and issues one command
    /// at a time, so replies are read inline (events in between are
    /// dropped — nothing here subscribes to any).
    pub struct Control {
        writer: UnixStream,
        reader: BufReader<UnixStream>,
        next_id: u64,
    }

    impl Control {
        /// Connect, answer the greeting with `qmp_capabilities` (QEMU
        /// rejects every other command before that) and return the
        /// session. A refused connection means the machine isn't running
        /// (or was started outside the launcher).
        pub fn connect(path: &Path) -> Result<Control, String> {
            let stream = UnixStream::connect(path).map_err(|e| format!("{}: {e}", path.display()))?;
            // Bounded so a wedged QEMU can't hang the UI thread forever;
            // generous because a snapshot command runs on QEMU's main
            // loop, which a busy guest can hold briefly.
            let timeout = Duration::from_secs(10);
            stream.set_read_timeout(Some(timeout)).map_err(|e| e.to_string())?;
            stream.set_write_timeout(Some(timeout)).map_err(|e| e.to_string())?;
            let reader = BufReader::new(stream.try_clone().map_err(|e| e.to_string())?);
            let mut control = Control { writer: stream, reader, next_id: 1 };
            control.read_until(|v| v.get("QMP").is_some())?; // the greeting
            control.execute("qmp_capabilities", Value::Null)?;
            Ok(control)
        }

        /// Read lines until `want` accepts one. Events and replies to
        /// commands we've stopped waiting for are skipped.
        fn read_until(&mut self, want: impl Fn(&Value) -> bool) -> Result<Value, String> {
            let mut line = String::new();
            loop {
                line.clear();
                match self.reader.read_line(&mut line) {
                    Ok(0) => return Err("monitor closed the connection".into()),
                    Ok(_) => {}
                    Err(e) => return Err(format!("reading the monitor: {e}")),
                }
                let Ok(v) = serde_json::from_str::<Value>(line.trim()) else {
                    continue;
                };
                if want(&v) {
                    return Ok(v);
                }
            }
        }

        /// Run one command and return its `return` payload, or QEMU's own
        /// error description — which is what a window should show
        /// ("Device 'ide1-cd0' is not removable" says more than a code).
        pub fn execute(&mut self, cmd: &str, args: Value) -> Result<Value, String> {
            let id = self.next_id;
            self.next_id += 1;
            let mut msg = json!({"execute": cmd, "id": id});
            if !args.is_null() {
                msg["arguments"] = args;
            }
            let mut line = msg.to_string();
            line.push('\n');
            self.writer.write_all(line.as_bytes()).map_err(|e| format!("writing to the monitor: {e}"))?;
            let reply = self.read_until(|v| v.get("id").and_then(Value::as_u64) == Some(id))?;
            if let Some(r) = reply.get("return") {
                return Ok(r.clone());
            }
            if let Some(e) = reply.get("error") {
                return Err(format!(
                    "{}: {}",
                    e.get("class").and_then(Value::as_str).unwrap_or("Error"),
                    e.get("desc").and_then(Value::as_str).unwrap_or("?")
                ));
            }
            Err(format!("malformed reply: {reply}"))
        }
    }
}

#[cfg(not(unix))]
mod imp {
    use serde_json::Value;
    use std::path::Path;

    /// Stub for a platform without Unix sockets (see the module docs):
    /// the socket is never created, so connecting always fails with the
    /// reason rather than the window pretending live control exists.
    pub struct Control;

    impl Control {
        pub fn connect(_path: &Path) -> Result<Control, String> {
            Err("live control needs a Unix socket (not available on this platform)".into())
        }

        pub fn execute(&mut self, _cmd: &str, _args: Value) -> Result<Value, String> {
            Err("live control needs a Unix socket (not available on this platform)".into())
        }
    }
}

pub use imp::Control;

/// The qdev id `bundle::Machine::qemu_args` gives the CD-ROM, so a live
/// medium change can name it.
pub const CDROM_ID: &str = "ide1-cd0";

impl Control {
    /// Put `disc` in the CD-ROM tray, replacing whatever is there.
    /// `blockdev-change-medium` does open/eject/insert/close as one
    /// command, which is what a guest expects to see from a disc swap.
    pub fn insert_disc(&mut self, disc: &Path) -> Result<(), String> {
        // No `format` argument: QEMU probes, so a `.cue`/`.ccd` still
        // lands on the `cdimage` driver (doc 17) exactly as it does on
        // the command line.
        self.execute(
            "blockdev-change-medium",
            // A folder goes in the drive the same way an image does, under
            // the prefix that makes it one (`disc_library::qemu_medium`).
            // No comma doubling here: this is a JSON string, not a QEMU
            // option string.
            serde_json::json!({"id": CDROM_ID, "filename": crate::disc_library::qemu_medium(disc)}),
        )
        .map(|_| ())
    }

    /// Open the tray and leave it empty.
    pub fn eject_disc(&mut self) -> Result<(), String> {
        self.execute("eject", serde_json::json!({"id": CDROM_ID, "force": true})).map(|_| ())
    }

    /// The block node holding `disk`, and the snapshots already in it.
    /// Snapshot commands address *node names*, which QEMU generates for
    /// a `-drive` (`#block123`) — so they're looked up rather than
    /// baked into `qemu_args`, which would pin an implementation detail
    /// of the command line into the bundle format.
    ///
    /// The match is on the filename QEMU reports; if that fails (a
    /// relative path in the bundle, a symlink resolved on the way in)
    /// the first writable qcow2 node stands in, which for our
    /// single-disk machines is the same node.
    pub fn disk_node(&mut self, disk: &Path) -> Result<(String, Vec<crate::snapshots::Snapshot>), String> {
        let nodes = self.execute("query-named-block-nodes", serde_json::Value::Null)?;
        let nodes = nodes.as_array().ok_or("query-named-block-nodes: not an array")?;
        let wanted = disk.display().to_string();
        // A qcow2 file shows up as *two* nodes — the qcow2 format node
        // and the `file` protocol node under it, both reporting the same
        // filename. Only the format node can hold a snapshot, so the
        // driver is part of the match, not just the name.
        let is_qcow2 = |n: &&Value| n["drv"].as_str() == Some("qcow2");
        let pick = nodes
            .iter()
            .find(|n| is_qcow2(n) && n["image"]["filename"].as_str() == Some(wanted.as_str()))
            .or_else(|| nodes.iter().find(|n| is_qcow2(n) && n["ro"].as_bool() != Some(true)))
            .ok_or_else(|| format!("the running machine has no qcow2 block node for {wanted}"))?;
        let name = pick["node-name"].as_str().ok_or("a block node without a node-name")?.to_string();
        Ok((name, crate::snapshots::parse(pick["image"].get("snapshots"))))
    }

    /// Start a snapshot job. `snapshot-save`/`-load`/`-delete` are
    /// *jobs*, not synchronous commands: they return as soon as the job
    /// is created and finish later (saving a 512 MB guest's RAM takes a
    /// visible moment), so the caller polls `job` below instead of the
    /// UI thread blocking on QEMU's main loop.
    pub fn start_snapshot_job(&mut self, command: &str, job_id: &str, tag: &str, node: &str) -> Result<(), String> {
        let mut args = serde_json::json!({"job-id": job_id, "tag": tag, "devices": [node]});
        if command != "snapshot-delete" {
            // The VM state goes in the same qcow2 as the disk, which is
            // what `savevm` does and what `qemu-img snapshot -a` (the
            // offline path) can then roll back to.
            args["vmstate"] = serde_json::Value::String(node.to_string());
        }
        self.execute(command, args).map(|_| ())
    }

    /// `(status, error)` for a job, or `None` once it's gone. A
    /// concluded job stays until dismissed, which is how its error is
    /// collected.
    pub fn job(&mut self, job_id: &str) -> Result<Option<(String, Option<String>)>, String> {
        let jobs = self.execute("query-jobs", serde_json::Value::Null)?;
        let Some(jobs) = jobs.as_array() else {
            return Ok(None);
        };
        Ok(jobs.iter().find(|j| j["id"].as_str() == Some(job_id)).map(|j| {
            (
                j["status"].as_str().unwrap_or("?").to_string(),
                j["error"].as_str().map(str::to_string),
            )
        }))
    }

    pub fn dismiss_job(&mut self, job_id: &str) -> Result<(), String> {
        self.execute("job-dismiss", serde_json::json!({"id": job_id})).map(|_| ())
    }

    /// Pause / resume the guest around a snapshot load: `snapshot-load`
    /// replaces the running machine's CPU and RAM state, and QEMU
    /// requires it to be stopped first.
    pub fn set_running(&mut self, run: bool) -> Result<(), String> {
        self.execute(if run { "cont" } else { "stop" }, serde_json::Value::Null).map(|_| ())
    }

    /// Whether the guest's CPUs are running — asked before a snapshot
    /// load stops them, so a machine the user had already paused isn't
    /// silently resumed afterwards.
    pub fn is_running(&mut self) -> Result<bool, String> {
        Ok(self.execute("query-status", serde_json::Value::Null)?["running"].as_bool().unwrap_or(false))
    }
}
