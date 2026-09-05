//! Companion launcher (doc 07): machine library, guided creation, disc
//! shelf. M6 skeleton: the library grid (`library.rs`) can spawn a player
//! (`player.rs`) and create or edit machines through a wizard
//! (`wizard.rs`) over the `machine.toml` bundle format (`bundle.rs`); no
//! thumbnails yet.

mod bundle;
mod filepicker;
mod library;
mod player;
mod shader_library;
mod shader_manager;
mod shader_profile;
mod wizard;

use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Child;

struct LauncherApp {
    library_dir: PathBuf,
    entries: Vec<library::LibraryEntry>,
    shader_profiles_dir: PathBuf,
    shader_profiles: Vec<shader_library::ProfileEntry>,
    /// Bundle directory -> its player process, while running. A bundle's
    /// absence here means "not running" (never tracked as ended-but-kept:
    /// `try_wait` removes it below the moment it exits).
    running: HashMap<PathBuf, Child>,
    wizard: wizard::Wizard,
    shader_manager: shader_manager::ShaderManager,
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
                    ui.strong("Shader");
                    ui.strong("Location");
                    ui.strong("");
                    ui.end_row();
                    for entry in &self.entries {
                        ui.label(&entry.machine.name);
                        ui.label(match entry.machine.family {
                            bundle::Family::Win98 => "Win98",
                            bundle::Family::Xp => "XP",
                        });
                        let shader_label = entry
                            .machine
                            .shader_profile
                            .as_deref()
                            .and_then(|id| self.shader_profiles.iter().find(|e| shader_library::id_of(&e.path) == id))
                            .map(|e| e.profile.name.clone())
                            .or_else(|| entry.machine.shader.as_ref().map(|p| p.display().to_string()))
                            .unwrap_or_else(|| "(default)".to_string());
                        ui.label(shader_label);
                        ui.label(entry.dir.display().to_string());
                        ui.horizontal(|ui| {
                            if self.running.contains_key(&entry.dir) {
                                ui.label("Running");
                            } else if ui.button("Play").clicked() {
                                match player::spawn(&entry.machine) {
                                    Ok(child) => {
                                        self.running.insert(entry.dir.clone(), child);
                                    }
                                    Err(e) => {
                                        eprintln!("[launcher] spawning player for {}: {e}", entry.dir.display())
                                    }
                                }
                            }
                            if ui.button("Edit…").clicked() {
                                self.wizard.open_edit(&entry.machine, entry.dir.join(library::BUNDLE_FILE));
                            }
                        });
                        ui.end_row();
                    }
                });
            }
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                if ui.button("New machine…").clicked() {
                    self.wizard.open_fresh();
                }
                if ui.button("Shader profiles…").clicked() {
                    self.shader_manager.open_list();
                }
            });
        });

        if let Some(_bundle_path) = self.wizard.show(&ctx, &self.library_dir, &self.shader_profiles) {
            self.entries = library::scan(&self.library_dir);
        }
        if self.shader_manager.show(&ctx, &self.shader_profiles_dir).is_some() {
            self.shader_profiles = shader_library::scan(&self.shader_profiles_dir);
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
            // "Browse…" button through an actual dialog. An optional arg
            // (a path field's current value, file or directory) exercises
            // the same start-directory extraction path_field itself uses.
            let start_dir = args.next().and_then(|v| filepicker::start_dir(&v));
            match filepicker::pick_file_headless(None, start_dir.as_deref()) {
                Some(path) => println!("{}", path.display()),
                None => println!("(cancelled)"),
            }
            return Ok(());
        }
        Some("--wizard-new") => {
            // Headless equivalent of the "New machine" window, for
            // scripted testing of the wizard's actual submit() logic
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
            let path = w.submit(&library::default_dir()).expect("create bundle");
            println!("{}", path.display());
            return Ok(());
        }
        Some("--wizard-edit") => {
            // Headless equivalent of clicking "Edit…" then "Save": loads
            // a bundle, renames it, saves it back in place, without a
            // GUI click.
            let usage = "usage: launcher --wizard-edit <machine.toml> <new-name>";
            let path = args.next().expect(usage);
            let new_name = args.next().expect(usage);
            let machine = bundle::Machine::load(std::path::Path::new(&path)).expect("load bundle");
            let mut w = wizard::Wizard::default();
            w.open_edit(&machine, path.clone().into());
            w.set_name(new_name);
            let saved = w.submit(&library::default_dir()).expect("save bundle");
            println!("{}", saved.display());
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
        Some("--new-shader-profile") => {
            let usage = "usage: launcher --new-shader-profile <name> <preset.slangp>";
            let name = args.next().expect(usage);
            let preset = args.next().expect(usage).into();
            let path = shader_library::create(&shader_library::default_dir(), name, preset).expect("create profile");
            println!("{}", path.display());
            return Ok(());
        }
        Some("--set-shader-param") => {
            let usage = "usage: launcher --set-shader-param <profile.toml> <param> <value>";
            let path: PathBuf = args.next().expect(usage).into();
            let param = args.next().expect(usage);
            let value: f32 = args.next().expect(usage).parse().expect("value must be a number");
            let mut profile = shader_profile::ShaderProfile::load(&path).expect("load profile");
            profile.params.insert(param, value);
            profile.save(&path).expect("save profile");
            println!("{}", profile.params_arg().unwrap_or_default());
            return Ok(());
        }
        Some("--list-shader-params") => {
            let preset = args.next().expect("usage: launcher --list-shader-params <preset.slangp>");
            let params = shader_profile::parameter_meta(std::path::Path::new(&preset)).expect("parse preset");
            for p in params {
                println!("{} [{}..{}] step {} = {} — {}", p.id, p.minimum, p.maximum, p.step, p.default, p.description);
            }
            return Ok(());
        }
        Some("--assign-shader") => {
            let usage = "usage: launcher --assign-shader <machine.toml> <profile-id-or-(none)>";
            let path: PathBuf = args.next().expect(usage).into();
            let id = args.next().expect(usage);
            let mut machine = bundle::Machine::load(&path).expect("load bundle");
            machine.shader_profile = if id == "(none)" { None } else { Some(id) };
            machine.save(&path).expect("save bundle");
            return Ok(());
        }
        Some("--print-shader-args") => {
            let path = args.next().expect("usage: launcher --print-shader-args <machine.toml>");
            let machine = bundle::Machine::load(std::path::Path::new(&path)).expect("load bundle");
            println!("{}", player::shader_args(&machine).join(" "));
            return Ok(());
        }
        _ => {}
    }

    let library_dir = library::default_dir();
    let entries = library::scan(&library_dir);
    let shader_profiles_dir = shader_library::default_dir();
    let shader_profiles = shader_library::scan(&shader_profiles_dir);
    eframe::run_native(
        "win98-xp-virt launcher",
        eframe::NativeOptions::default(),
        Box::new(|_cc| {
            Ok(Box::new(LauncherApp {
                library_dir,
                entries,
                shader_profiles_dir,
                shader_profiles,
                running: HashMap::new(),
                wizard: wizard::Wizard::default(),
                shader_manager: shader_manager::ShaderManager::default(),
            }))
        }),
    )
}
