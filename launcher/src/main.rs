//! Companion launcher (doc 07): machine library, guided creation, disc
//! shelf. M6 skeleton: the library grid (`library.rs`) can spawn a player
//! (`player.rs`) and create new machines through a wizard (`wizard.rs`)
//! over the `machine.toml` bundle format (`bundle.rs`); no thumbnails yet.

mod bundle;
mod filepicker;
mod library;
mod player;
mod wizard;

use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Child;

struct LauncherApp {
    library_dir: PathBuf,
    entries: Vec<library::LibraryEntry>,
    /// Bundle directory -> its player process, while running. A bundle's
    /// absence here means "not running" (never tracked as ended-but-kept:
    /// `try_wait` removes it below the moment it exits).
    running: HashMap<PathBuf, Child>,
    wizard: wizard::Wizard,
}

impl eframe::App for LauncherApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        // No push notification from a child exiting: poll for it instead.
        ctx.request_repaint_after(std::time::Duration::from_millis(500));
        self.running.retain(|dir, child| match child.try_wait() {
            Ok(None) => true,
            Ok(Some(status)) => {
                eprintln!("[launcher] {} exited: {status}", dir.display());
                false
            }
            Err(e) => {
                eprintln!("[launcher] {}: {e}", dir.display());
                false
            }
        });

        egui::CentralPanel::default().show(ui, |ui| {
            ui.heading("win98-xp-virt");
            ui.add_space(8.0);
            if self.entries.is_empty() {
                ui.label("No machines yet.");
                ui.label(format!("Library: {}", self.library_dir.display()));
            } else {
                egui::Grid::new("library").striped(true).show(ui, |ui| {
                    ui.strong("Name");
                    ui.strong("Family");
                    ui.strong("Location");
                    ui.strong("");
                    ui.end_row();
                    for entry in &self.entries {
                        ui.label(&entry.machine.name);
                        ui.label(match entry.machine.family {
                            bundle::Family::Win98 => "Win98",
                            bundle::Family::Xp => "XP",
                        });
                        ui.label(entry.dir.display().to_string());
                        if self.running.contains_key(&entry.dir) {
                            ui.label("Running");
                        } else if ui.button("Play").clicked() {
                            match player::spawn(&entry.machine) {
                                Ok(child) => {
                                    self.running.insert(entry.dir.clone(), child);
                                }
                                Err(e) => eprintln!("[launcher] spawning player for {}: {e}", entry.dir.display()),
                            }
                        }
                        ui.end_row();
                    }
                });
            }
            ui.add_space(8.0);
            if ui.button("New machine…").clicked() {
                self.wizard.open_fresh();
            }
        });

        if let Some(_bundle_path) = self.wizard.show(&ctx, &self.library_dir) {
            self.entries = library::scan(&self.library_dir);
        }
    }
}

fn main() -> eframe::Result {
    // Debug/advanced-drawer aids (doc 07), until the wizard exists:
    // `--new` bootstraps a bundle into the library from the doc 06
    // reference defaults, `--print-args` shows the qemu-system-i386
    // command line a bundle translates to. Neither opens a window.
    let mut args = std::env::args().skip(1);
    match args.next().as_deref() {
        Some("--print-args") => {
            let path = args.next().expect("usage: launcher --print-args <machine.toml>");
            let machine = bundle::Machine::load(std::path::Path::new(&path)).expect("load bundle");
            println!("{}", machine.qemu_args(&player::pc_bios_dir()).join(" "));
            return Ok(());
        }
        Some("--play") => {
            let path = args.next().expect("usage: launcher --play <machine.toml>");
            let machine = bundle::Machine::load(std::path::Path::new(&path)).expect("load bundle");
            let child = player::spawn(&machine).expect("spawn player");
            println!("pid {}", child.id());
            return Ok(());
        }
        Some("--pick-file") => {
            // Exercises the real OS file dialog (rfd) headlessly — proof
            // the portal/NSOpenPanel/IFileDialog wiring works, since this
            // session has no GUI click automation to drive the wizard's
            // "Browse…" button through an actual dialog.
            match filepicker::pick_file_headless(None) {
                Some(path) => println!("{}", path.display()),
                None => println!("(cancelled)"),
            }
            return Ok(());
        }
        Some("--wizard-new") => {
            // Headless equivalent of the "New machine" window, for
            // scripted testing of the wizard's actual create() logic
            // (disk creation via qemu-img included) without a GUI click.
            let usage = "usage: launcher --wizard-new <win98|xp> <name> <disk-size-gb>";
            let family = match args.next().as_deref() {
                Some("win98") => bundle::Family::Win98,
                Some("xp") => bundle::Family::Xp,
                _ => panic!("{usage}"),
            };
            let name = args.next().expect(usage);
            let size_gb: u32 = args.next().expect(usage).parse().expect("disk size must be a number");
            let w = wizard::Wizard::with_new_disk(family, name, size_gb);
            let path = w.create(&library::default_dir()).expect("create bundle");
            println!("{}", path.display());
            return Ok(());
        }
        Some("--new") => {
            let usage = "usage: launcher --new <win98|xp> <name> <disk.qcow2>";
            let family = match args.next().as_deref() {
                Some("win98") => bundle::Family::Win98,
                Some("xp") => bundle::Family::Xp,
                _ => panic!("{usage}"),
            };
            let name = args.next().expect(usage);
            let disk = args.next().expect(usage).into();
            let path = library::create(&library::default_dir(), family, name, disk).expect("create bundle");
            println!("{}", path.display());
            return Ok(());
        }
        _ => {}
    }

    let library_dir = library::default_dir();
    let entries = library::scan(&library_dir);
    eframe::run_native(
        "win98-xp-virt launcher",
        eframe::NativeOptions::default(),
        Box::new(|_cc| {
            Ok(Box::new(LauncherApp {
                library_dir,
                entries,
                running: HashMap::new(),
                wizard: wizard::Wizard::default(),
            }))
        }),
    )
}
