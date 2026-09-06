//! The disc shelf's model (doc 07: "disc shelf editing, one-click
//! guest-tools ISO attach").
//!
//! One object, two modes. Opened on its own it manages the shared shelf
//! (`disc_library.rs`) — add, label, remove. Opened from a machine it
//! carries that machine's two disc decisions as well: which disc is in
//! the drive at **boot** (a bundle edit, `Machine::disc`) and — while
//! the machine is running — which disc to **insert** right now (a
//! monitor command, `control.rs`). The shelf is deliberately not
//! filtered per machine: it is the user's collection, and any disc can
//! go in any drive.
//!
//! Library edits save as they are made; there is no "Save" button,
//! because a shelf is a list of things you own, not a document being
//! drafted. *When* they are written is the one thing the two front ends
//! genuinely differ on and so is left to them: an immediate-mode label
//! field would write the file on every keystroke, so the egui build sets
//! `dirty` while drawing and calls `flush` at the end of the frame,
//! while Qt's `TextField` has an `editingFinished` and calls `set_label`
//! then `flush` once. Both go through the same two methods.

use crate::bundle::Machine;
use crate::control;
use crate::disc_library::{self, Disc, DiscLibrary};
use std::path::{Path, PathBuf};

/// What the window needs when it was opened for a particular machine.
struct MachineContext {
    bundle_path: PathBuf,
    name: String,
    /// The machine's current boot disc, mirrored here so a click updates
    /// the row markers before the library is rescanned.
    boot: Option<PathBuf>,
}

#[derive(Default)]
pub struct Shelf {
    /// Whether the window is up.
    pub open: bool,
    library_path: PathBuf,
    library: DiscLibrary,
    /// The shelf changed and needs writing back.
    dirty: bool,
    /// Set by `flush` when it actually wrote; see `take_saved`.
    saved: bool,
    /// `None` when the window was opened for the shelf itself rather
    /// than for one machine.
    machine: Option<MachineContext>,
    status: Option<String>,
    error: Option<String>,
}

impl Shelf {
    /// Open on the shared shelf alone, with no machine context.
    pub fn open_library(&mut self, library_path: &Path) {
        *self = Shelf { open: true, ..Default::default() };
        self.load(library_path);
    }

    /// Open for one machine: the same shelf, plus its boot-disc choice
    /// and (while running) live insert.
    pub fn open_for(&mut self, machine: &Machine, bundle_path: PathBuf, library_path: &Path) {
        *self = Shelf {
            open: true,
            machine: Some(MachineContext {
                bundle_path,
                name: machine.name.clone(),
                boot: machine.boot_disc().cloned(),
            }),
            ..Default::default()
        };
        self.load(library_path);
    }

    /// The same, from a bundle path — for a front end that addresses its
    /// windows by path rather than by a `Machine` it is already holding.
    /// A bundle that won't load still opens the shelf (the shared half
    /// is perfectly usable), with the reason in `error`.
    pub fn open_for_path(&mut self, bundle_path: PathBuf, library_path: &Path) {
        match Machine::load(&bundle_path) {
            Ok(machine) => self.open_for(&machine, bundle_path, library_path),
            Err(e) => {
                let message = format!("{}: {e}", bundle_path.display());
                self.open_library(library_path);
                self.error = Some(message);
            }
        }
    }

    fn load(&mut self, library_path: &Path) {
        self.library_path = library_path.to_path_buf();
        match DiscLibrary::load(library_path) {
            Ok(library) => self.library = library,
            // A corrupt shelf is reported, never silently replaced with
            // an empty one — the next save would then destroy it.
            Err(e) => self.error = Some(format!("{}: {e}", library_path.display())),
        }
    }

    /// The window's title: the machine's name when it has one.
    pub fn title(&self) -> String {
        match &self.machine {
            Some(m) => format!("Discs — {}", m.name),
            None => "Disc shelf".to_string(),
        }
    }

    pub fn for_machine(&self) -> bool {
        self.machine.is_some()
    }

    pub fn discs(&self) -> &[Disc] {
        &self.library.discs
    }

    /// The rows, editable in place — an immediate-mode label field
    /// writes straight into one and calls `mark_dirty`.
    pub fn discs_mut(&mut self) -> &mut [Disc] {
        &mut self.library.discs
    }

    pub fn mark_dirty(&mut self) {
        self.dirty = true;
    }

    /// Rename one row. The retained-mode path: the field reports a
    /// finished edit and this is what it calls.
    pub fn set_label(&mut self, row: usize, label: &str) {
        let Some(disc) = self.library.discs.get_mut(row) else { return };
        if disc.label == label {
            return;
        }
        disc.label = label.to_string();
        self.dirty = true;
    }

    /// The machine's boot disc, or `None` for an empty tray.
    pub fn boot(&self) -> Option<&Path> {
        self.machine.as_ref().and_then(|m| m.boot.as_deref())
    }

    /// The label for it, ready to print.
    pub fn boot_label(&self) -> String {
        self.boot().map(disc_library::default_label).unwrap_or_else(|| "(empty tray)".to_string())
    }

    /// Write pending shelf edits now.
    pub fn flush(&mut self) -> std::io::Result<()> {
        if std::mem::take(&mut self.dirty) {
            self.library.save(&self.library_path)?;
            self.saved = true;
        }
        Ok(())
    }

    /// `flush`, with any failure folded into `error` rather than
    /// returned — what a front end wants at the end of a frame or a
    /// handler, where there is nobody to return an error to.
    pub fn flush_reporting(&mut self) {
        if let Err(e) = self.flush() {
            self.error = Some(e.to_string());
        }
    }

    /// Whether the shelf was written since this was last asked — the cue
    /// to republish it to any running machine's drive so the in-guest
    /// CDSHELF program sees a disc the moment it is added.
    pub fn take_saved(&mut self) -> bool {
        std::mem::take(&mut self.saved)
    }

    /// The directory of the bundle this window has open, so the caller
    /// can tell whether *that* machine is the running one.
    pub fn bundle_dir(&self) -> Option<&Path> {
        self.machine.as_ref().and_then(|m| m.bundle_path.parent())
    }

    /// Put a disc on the shelf.
    pub fn add(&mut self, path: PathBuf) {
        let label = disc_library::default_label(&path);
        if self.library.add(path) {
            self.dirty = true;
            self.status = Some(format!("added {label}"));
            self.error = None;
        } else {
            // The same image added twice is one entry, not two rows that
            // then disagree about their labels.
            self.status = Some(format!("{label} is already on the shelf"));
        }
    }

    /// Doc 07's one-click guest-tools attach: no path to find, no
    /// browsing — the driver/test ISO this checkout last built.
    pub fn add_guest_tools(&mut self) {
        match disc_library::guest_tools_iso() {
            Some(iso) => self.add(iso),
            None => self.error = Some("no guest-tools ISO built (guest-tools/build-wrappers.sh)".into()),
        }
    }

    pub fn remove(&mut self, path: &Path) {
        if let Some(i) = self.library.position(path) {
            let disc = self.library.discs.remove(i);
            self.dirty = true;
            self.status = Some(format!("removed {}", disc.label));
            self.error = None;
        }
    }

    pub fn remove_row(&mut self, row: usize) {
        if let Some(path) = self.library.discs.get(row).map(|d| d.path.clone()) {
            self.remove(&path);
        }
    }

    /// Set (or clear) the open machine's boot disc, writing the bundle.
    /// Returns the bundle path on success so the caller can rescan.
    pub fn set_boot(&mut self, path: Option<PathBuf>) -> Option<PathBuf> {
        let machine = self.machine.as_mut()?;
        // Re-read rather than editing a `Machine` captured when the
        // window opened: the wizard may have saved the same bundle in
        // between, and only the boot disc is this window's to change.
        let result = Machine::load(&machine.bundle_path).and_then(|mut m| {
            m.disc = path.clone();
            m.discs.clear();
            m.save(&machine.bundle_path)
        });
        match result {
            Ok(()) => {
                machine.boot = path.clone();
                self.status = Some(match &path {
                    Some(p) => format!("boots with {}", disc_library::default_label(p)),
                    None => "boots with an empty tray".to_string(),
                });
                self.error = None;
                Some(self.machine.as_ref()?.bundle_path.clone())
            }
            Err(e) => {
                self.error = Some(e.to_string());
                None
            }
        }
    }

    pub fn set_boot_row(&mut self, row: usize) -> Option<PathBuf> {
        let path = self.library.discs.get(row).map(|d| d.path.clone())?;
        self.set_boot(Some(path))
    }

    /// Swap `disc` into the running machine's drive.
    pub fn insert_live(&mut self, disc: &Path) {
        let name = disc.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let disc = disc.to_path_buf();
        self.live(|c| c.insert_disc(&disc), &format!("inserted {name}"));
    }

    pub fn insert_live_row(&mut self, row: usize) {
        if let Some(path) = self.library.discs.get(row).map(|d| d.path.clone()) {
            self.insert_live(&path);
        }
    }

    pub fn eject_live(&mut self) {
        self.live(|c| c.eject_disc(), "ejected");
    }

    /// Republish the shelf to one running machine's drive file, so the
    /// in-guest CDSHELF program sees the current list.
    pub fn publish_to(&self, bundle_dir: &Path) {
        crate::machines::publish_shelf(&self.library_path, &control::shelf_path(bundle_dir));
    }

    pub fn status(&self) -> Option<&str> {
        self.status.as_deref()
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    /// The last operation's result: the status line, or the error.
    pub fn last_result(&self) -> Result<Option<&str>, &str> {
        match &self.error {
            Some(e) => Err(e),
            None => Ok(self.status.as_deref()),
        }
    }

    /// Run one operation on the running machine's monitor. A fresh
    /// connection each time (jobs and block nodes are QEMU-global, so
    /// nothing is lost, and there is no half-open socket to nurse);
    /// failures land in the error line rather than a panic — the guest
    /// may have shut down between the repaint that drew the button and
    /// the click on it.
    fn live(&mut self, op: impl FnOnce(&mut control::Control) -> Result<(), String>, done: &str) {
        let Some(dir) = self.bundle_dir() else {
            self.status = None;
            self.error = Some("no machine open".into());
            return;
        };
        let result = control::Control::connect(&control::socket_path(dir)).and_then(|mut c| op(&mut c));
        match result {
            Ok(()) => {
                self.status = Some(done.to_string());
                self.error = None;
            }
            Err(e) => {
                self.status = None;
                self.error = Some(e);
            }
        }
    }
}
